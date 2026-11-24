"""Export the public flx DeepPrint model and record Python reference vectors."""

import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import sys

import numpy as np
import onnx
import onnxruntime as ort
from PIL import Image
import torch
from torch import nn
from torch.nn import functional as F


UPSTREAM_REVISION = "7accfca1f33b9b42bfd220e43cd5bc13b4a7fa13"
CHECKPOINT_SHA256 = "2683a04427bacd54adc00cfdc97474625b1e11e5a9e6672c5129f033018f8a28"
INPUT_SIZE = 299


def normalize_image(image):
    low = image.amin(dim=(-2, -1), keepdim=True)
    high = image.amax(dim=(-2, -1), keepdim=True)
    span = high - low
    return (image - low) / torch.where(span > 0, span, torch.ones_like(span))


class GaborResponse(nn.Module):
    def __init__(self, pattern):
        super().__init__()
        self.register_buffer("pattern", pattern)
        self.margin = pattern.shape[-1] // 2

    def forward(self, image):
        response = (-F.conv2d(image, self.pattern).amin(dim=1, keepdim=True)).clamp_min(0)
        return F.pad(response, (self.margin,) * 4)


class FingerprintBinarization(nn.Module):
    """Tensor operations equivalent to flx's FVC binarizer for 299x299 inputs."""

    def __init__(self, reference):
        super().__init__()
        self.responses = nn.ModuleList(GaborResponse(f._pattern) for f in reference._gabor_filters)

        size = reference._blur_kernel_size * 7
        sigma = reference._blur_kernel_sigma * 7
        self.blur_margin = size // 2
        axis = torch.arange(size, dtype=torch.float32) - self.blur_margin
        gaussian = torch.exp(-0.5 * (axis / sigma).square())
        gaussian = gaussian / gaussian.sum()
        self.register_buffer("blur_kernel", torch.outer(gaussian, gaussian)[None, None])

        # Record PyTorch's antialiased resize coefficients as constants.
        # MatMul expresses the separable resize without an exporter-specific operator.
        resized = int(INPUT_SIZE * 0.9)
        basis = torch.eye(INPUT_SIZE).reshape(INPUT_SIZE, 1, 1, INPUT_SIZE)
        weights = F.interpolate(
            basis, size=(1, resized), mode="bilinear", align_corners=False, antialias=True
        ).reshape(INPUT_SIZE, resized)
        self.register_buffer("resize_weights", weights.contiguous())
        before = (INPUT_SIZE - resized) // 2
        self.segment_padding = (before, INPUT_SIZE - resized - before) * 2

    def forward(self, image):
        image = normalize_image(image)
        inverted = 1.0 - image
        ridges = self.responses[0](image)
        valleys = self.responses[0](-image)
        for response in self.responses[1:]:
            ridges = torch.maximum(ridges, response(image))
            valleys = torch.maximum(valleys, response(inverted))
        ridges, valleys = normalize_image(ridges), normalize_image(valleys)
        pixels = F.pad(ridges + valleys, (self.blur_margin,) * 4, mode="reflect")
        segmentation = (F.conv2d(pixels, self.blur_kernel) > 0.1).to(image.dtype)
        segmentation = self.resize_weights.T @ segmentation @ self.resize_weights
        segmentation = F.pad(segmentation, self.segment_padding)
        differences = ridges - valleys
        contrast = (8.0 * differences - 7.0 * differences.mean(dim=(-2, -1), keepdim=True))
        return (contrast.clamp(0, 1) * segmentation > 0.2).to(image.dtype)


class DeepPrintInference(nn.Module):
    def __init__(self, model, binarizer):
        super().__init__()
        self.binarize = FingerprintBinarization(binarizer)
        self.stem = model.stem
        # The original forward squeezes away the batch axis for a single image.
        self.texture = nn.Sequential(*model.texture_branch.children())
        self.minutia_stem = model.minutia_stem
        self.minutia_embedding = model.minutia_embedding

    def forward(self, image):
        features = self.stem(self.binarize(image))
        texture = F.normalize(self.texture(features), dim=1)
        minutiae = self.minutia_embedding(self.minutia_stem(features))
        return torch.cat((texture, minutiae), dim=1)


def file_digest(path):
    with path.open("rb") as source:
        return hashlib.file_digest(source, "sha256").hexdigest()


def load_reference(args):
    revision = subprocess.check_output(
        ["git", "-C", args.flx, "rev-parse", "HEAD"], text=True
    ).strip()
    if revision != UPSTREAM_REVISION:
        raise ValueError(f"Expected flx commit {UPSTREAM_REVISION}, got {revision}")
    if subprocess.check_output(["git", "-C", args.flx, "status", "--porcelain"]).strip():
        raise ValueError("Expected a clean upstream flx checkout")
    if file_digest(args.checkpoint) != CHECKPOINT_SHA256:
        raise ValueError("Unexpected flx checkpoint SHA-256")

    sys.path.insert(0, str(args.flx.resolve()))
    from flx.image_processing.binarization import _FingerprintBinarizer
    from flx.models.deep_print_arch import DeepPrint_TexMinu

    state = torch.load(args.checkpoint, map_location="cpu", weights_only=True, mmap=True)[
        "model_state_dict"
    ]
    classes = state["texture_logits.0.weight"].shape[0]
    model = DeepPrint_TexMinu(classes, 256, 256).eval()
    model.load_state_dict(state, strict=True)
    # This ridge width is used by flx's FVC2004 evaluation loader.
    return model, _FingerprintBinarizer(1.8)


def representative_images(root):
    types = {(448, 478): 1, (300, 300): 2, (640, 480): 3}
    seen = set()
    for folder in sorted(root.iterdir()):
        if not folder.is_dir():
            continue
        files = sorted(p for p in folder.iterdir() if p.suffix.lower() in (".tif", ".tiff"))
        if not files:
            continue
        with Image.open(files[0]) as source:
            image_type = types[source.size]
            pixels = np.array(source.convert("L"))
        if image_type not in seen:
            seen.add(image_type)
            yield image_type, files[0], pixels
    if seen != set(types.values()):
        raise ValueError("The reference dataset must contain all three FVC image sizes")


def export(args):
    torch.set_num_threads(4)
    model, binarizer = load_reference(args)
    inference = DeepPrintInference(model, binarizer).eval()
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.reference_dir.mkdir(parents=True, exist_ok=True)
    from flx.data.image_helpers import pad_and_resize_to_deepprint_input_size

    references = []
    with torch.no_grad():
        for image_type, path, pixels in representative_images(args.fvc):
            image = pad_and_resize_to_deepprint_input_size(pixels, fill=1.0).unsqueeze(0)
            filtered = binarizer.binarize(image[0])
            torch.testing.assert_close(inference.binarize(image)[0], filtered)
            # The unmodified upstream texture branch requires a batch of at least two.
            upstream = model(torch.stack((filtered, filtered)))
            expected = torch.cat(
                (upstream.texture_embeddings[:1], upstream.minutia_embeddings[:1]), dim=1
            )
            torch.testing.assert_close(inference(image), expected, atol=5e-5, rtol=5e-5)
            references.append((image_type, path, pixels, image, expected.numpy()))

        temporary = args.output.with_suffix(".tmp.onnx")
        try:
            torch.onnx.export(
                inference, references[0][3], temporary, opset_version=17, dynamo=False,
                input_names=["image"], output_names=["embedding"],
            )
            onnx.checker.check_model(str(temporary))
            options = ort.SessionOptions()
            options.intra_op_num_threads = 4
            session = ort.InferenceSession(
                str(temporary), sess_options=options, providers=["CPUExecutionProvider"]
            )
            for image_type, path, pixels, image, expected in references:
                actual = session.run(["embedding"], {"image": image.numpy()})[0]
                np.testing.assert_allclose(actual, expected, rtol=5e-5, atol=5e-5)
                print(f"iType {image_type} ({path.parent.name}/{path.name}): "
                      f"ONNX max absolute error {np.max(np.abs(actual - expected)):.3g}")
                pixels.tofile(args.reference_dir / f"{image_type}.raw")
                image.numpy().astype("<f4").tofile(args.reference_dir / f"{image_type}.input.f32")
                expected.astype("<f4").tofile(args.reference_dir / f"{image_type}.embedding.f32")
            del session  # Release the model handle before renaming on Windows.
            temporary.replace(args.output)
        finally:
            temporary.unlink(missing_ok=True)

    metadata = {
        "upstream_revision": UPSTREAM_REVISION,
        "checkpoint_sha256": CHECKPOINT_SHA256,
        "onnx_sha256": file_digest(args.output),
        "input": [1, 1, INPUT_SIZE, INPUT_SIZE],
        "embedding": ["texture:256", "minutiae:256"],
        "binarization": {"ridge_width": 1.8},
    }
    args.output.with_suffix(".json").write_text(json.dumps(metadata, indent=2) + "\n")
    print(f"Saved verified model to {args.output}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--flx", type=Path, required=True)
    parser.add_argument("--checkpoint", type=Path, required=True)
    parser.add_argument("--fvc", type=Path, default=Path("data/fvc"))
    parser.add_argument("--output", type=Path, default=Path("build/models/deepprint_texminu_512.onnx"))
    parser.add_argument("--reference-dir", type=Path, default=Path("build/models/parity"))
    export(parser.parse_args())


if __name__ == "__main__":
    main()
