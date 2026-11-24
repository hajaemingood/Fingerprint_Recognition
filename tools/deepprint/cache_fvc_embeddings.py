"""Cache fixed DeepPrint embeddings for reproducible FVC adaptation experiments."""

import argparse
import csv
from concurrent.futures import ThreadPoolExecutor
import hashlib
import json
from pathlib import Path
import time

import numpy as np
import onnxruntime as ort
from PIL import Image
import torch
import torchvision.transforms.functional as vision


DATABASES = ("FVC2002_DB2_B", "FVC2004_DB1_B")
PILOTS = {
    "FVC2002_DB2_B": {
        "bilateral": ("original", "noise-10-s42", "noise-40-s42"),
    },
    "FVC2004_DB1_B": {
        "center": ("original", "rotation-10", "rotation+10"),
        "roi": ("original", "rotation-10", "rotation+10"),
    },
}


def file_hash(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def read_rows(path):
    with path.open(encoding="utf-8-sig", newline="") as stream:
        return list(csv.DictReader(stream))


def condition_key(row):
    value = int(row.get("parameter", row.get("sigma", 0)))
    kind = row.get("transform", "original" if value == 0 else "gaussian")
    if kind == "gaussian":
        return f"noise-{value:02}-s{int(row['base_seed']):02}"
    if kind == "original":
        return kind
    return f"{kind}{value:+03}"


def paths_for_conditions(data, augmentation):
    groups = {}
    for database in DATABASES:
        originals = sorted((data / database).glob("*.tif"))
        if not originals:
            raise ValueError(f"No original TIFFs for {database}")
        groups[database] = {"original": {f.name: f for f in originals}}

    for directory in augmentation:
        for row in read_rows(directory / "manifest.csv"):
            database = row["database"]
            if database not in groups:
                continue
            condition = condition_key(row)
            if condition == "original" or condition.startswith("flip"):
                continue
            mapping = groups[database].setdefault(condition, {})
            filename = Path(row["source"]).name
            if filename in mapping:
                raise ValueError(f"Duplicate variant {database}/{condition}/{filename}")
            mapping[filename] = directory / row["probe"]

    expected = {"original"}
    expected.update(f"noise-{sigma:02}-s{seed}" for sigma in (2, 5, 10, 20, 30, 40) for seed in (42, 43))
    expected.update(f"rotation{value:+03}" for value in (-10, -5, 5, 10))
    expected.update(f"brightness{value:+03}" for value in (-15, 15))
    for database, conditions in groups.items():
        if set(conditions) != expected:
            raise ValueError(f"Unexpected conditions for {database}: {sorted(set(conditions) ^ expected)}")
        originals = conditions["original"].keys()
        for condition, mapping in conditions.items():
            if mapping.keys() != originals or any(not path.is_file() for path in mapping.values()):
                raise ValueError(f"Incomplete TIFF coverage for {database}/{condition}")
    return groups


def bilateral(image):
    pixels = image.astype(np.float32)
    height, width = pixels.shape
    padded = np.pad(pixels, 1, mode="edge")
    weighted = np.zeros_like(pixels)
    total = np.zeros_like(pixels)
    for y in range(3):
        for x in range(3):
            shifted = padded[y:y + height, x:x + width]
            intensity = (shifted - pixels) / 12.0
            weight = np.exp(-0.5 * ((x - 1) ** 2 + (y - 1) ** 2 + intensity * intensity))
            weighted += shifted * weight
            total += weight
    return np.rint(weighted / total).astype(np.uint8)


def foreground_box(image):
    height, width = image.shape
    block = 16
    view = image[:height // block * block, :width // block * block]
    cells = view.reshape(height // block, block, width // block, block)
    foreground = (cells.std(axis=(1, 3)) >= 12) & ((cells < 180).mean(axis=(1, 3)) >= 0.08)
    rows = np.flatnonzero(foreground.sum(axis=1) >= 2)
    columns = np.flatnonzero(foreground.sum(axis=0) >= 2)
    if len(rows) < 4 or len(columns) < 4:
        return None
    return (columns[0] * block, rows[0] * block, (columns[-1] + 1) * block, (rows[-1] + 1) * block)


def preprocess(image, mode):
    if mode == "bilateral":
        return bilateral(image)
    if mode == "standard":
        return image

    box = foreground_box(image)
    if box is None:
        return image
    left, top, right, bottom = box
    height, width = image.shape
    if mode == "roi":
        pad_x = round((right - left) * 0.10)
        pad_y = round((bottom - top) * 0.10)
        return image[max(0, top - pad_y):min(height, bottom + pad_y),
                     max(0, left - pad_x):min(width, right + pad_x)]
    if mode != "center":
        raise ValueError(f"Unknown preprocessing mode: {mode}")

    dx = (width - left - right) // 2
    dy = (height - top - bottom) // 2
    centered = np.full_like(image, 255)
    x0, x1 = max(0, -dx), min(width, width - dx)
    y0, y1 = max(0, -dy), min(height, height - dy)
    centered[y0 + dy:y1 + dy, x0 + dx:x1 + dx] = image[y0:y1, x0:x1]
    return centered


def prepare_image(path, mode):
    with Image.open(path) as source:
        image = np.array(source.convert("L"))
    image = preprocess(image, mode)
    height, width = image.shape
    side = max(height, width)
    top, left = (side - height) // 2, (side - width) // 2
    tensor = vision.to_tensor(image)
    tensor = vision.pad(tensor, [left, top, side - width - left, side - height - top], fill=1.0)
    return vision.resize(tensor, [299, 299], antialias=True).unsqueeze(0).contiguous().numpy()


def run_cache(args):
    torch.set_num_threads(1)
    groups = paths_for_conditions(args.fvc, args.augmentation)
    description = {
        "model": str(args.model.resolve()),
        "model_sha256": file_hash(args.model),
        "fvc": str(args.fvc.resolve()),
        "manifests": {str(p.resolve()): file_hash(p / "manifest.csv") for p in args.augmentation},
        "preprocessing_pilot": not args.standard_only,
    }
    args.output.mkdir(parents=True, exist_ok=True)
    metadata = args.output / "source.json"
    if metadata.exists() and json.loads(metadata.read_text(encoding="utf-8")) != description:
        raise ValueError("Cache inputs changed. Choose a new output directory.")
    metadata.write_text(json.dumps(description, indent=2), encoding="utf-8")

    tasks = []
    for database, conditions in groups.items():
        for condition, mapping in sorted(conditions.items()):
            tasks.append((database, "standard", condition, mapping))
        if not args.standard_only:
            for mode, subset in PILOTS[database].items():
                tasks.extend((database, mode, condition, conditions[condition]) for condition in subset)

    options = ort.SessionOptions()
    options.intra_op_num_threads = args.threads
    options.inter_op_num_threads = 1
    session = ort.InferenceSession(str(args.model), sess_options=options, providers=["CPUExecutionProvider"])

    def extract(path, mode):
        start = time.perf_counter()
        tensor = prepare_image(path, mode)
        vector = session.run(["embedding"], {"image": tensor})[0].reshape(-1)
        if vector.size != 512 or not np.isfinite(vector).all():
            raise ValueError(f"Invalid DeepPrint embedding: {path}")
        return vector, (time.perf_counter() - start) * 1000

    with ThreadPoolExecutor(max_workers=args.workers) as pool:
        for index, (database, mode, condition, mapping) in enumerate(tasks, start=1):
            destination = args.output / database / mode / f"{condition}.npz"
            if destination.exists():
                print(f"[{index}/{len(tasks)}] {database}/{mode}/{condition} cached", flush=True)
                continue
            names = sorted(mapping)
            vectors, timings = zip(*pool.map(lambda name: extract(mapping[name], mode), names))
            destination.parent.mkdir(parents=True, exist_ok=True)
            temporary = destination.with_suffix(".tmp")
            with temporary.open("wb") as stream:
                np.savez(stream, names=names, embeddings=np.stack(vectors), attempt_ms=timings)
            temporary.replace(destination)
            print(f"[{index}/{len(tasks)}] {database}/{mode}/{condition}: {len(names)} embeddings", flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--model", type=Path, required=True)
    parser.add_argument("--fvc", type=Path, required=True)
    parser.add_argument("--augmentation", type=Path, action="append", required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--threads", type=int, default=4)
    parser.add_argument("--workers", type=int, default=2)
    parser.add_argument("--standard-only", action="store_true")
    run_cache(parser.parse_args())


if __name__ == "__main__":
    main()
