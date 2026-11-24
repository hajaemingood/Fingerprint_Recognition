"""Fine-tune on six captures per FVC ID with the highest ID held out."""

import argparse
import ctypes
from dataclasses import dataclass
import json
from pathlib import Path
import shutil
import statistics
import sys

import numpy as np
from PIL import Image
import torch

if sys.flags.isolated:
    sys.path.insert(0, str(Path(__file__).resolve().parent))

from cache_fvc_embeddings import MAX_SIGMA, PROFILE, augmentation_groups, file_hash, paths_for_conditions
from finetune_fvc_heads import (
    MAX_EPOCHS,
    TEXTURE_WEIGHT,
    choose_threshold,
    compare_vectors,
    eer,
    export_head_candidate,
    fit,
    load_banks,
    normalized_vectors,
    write_csv,
)


SEED = 42
# The deployed weight was selected using one of the IDs held out in this experiment.
EXPERIMENT_WEIGHT = 0.50
AUGMENTATION_GROUPS = augmentation_groups()
CONDITIONS = ("original", *(key for group in AUGMENTATION_GROUPS for key in group))
NATIVE_SUBSETS = {
    "original": ("original",),
    "maximum_noise": tuple(f"noise-{MAX_SIGMA:02}-s{seed}" for seed in PROFILE["gaussian_seeds"]),
}


@dataclass
class Split:
    held_id: int
    training: np.ndarray
    inner_training: np.ndarray
    pair_inputs: dict
    rows: list


def make_split(database, bank):
    ids = np.unique(bank.fingers)
    if len(ids) != 10 or any(np.count_nonzero(bank.fingers == finger) != 8 for finger in ids):
        raise ValueError(f"{database} requires ten IDs with eight captures each")
    random = np.random.default_rng(SEED)
    seen = [random.permutation(np.flatnonzero(bank.fingers == finger)) for finger in ids[:-1]]
    seen = np.stack(seen)
    unseen = np.flatnonzero(bank.fingers == ids[-1])
    training = seen[:, :6].flatten()
    inner_training = seen[:, :5].flatten()
    pair_inputs = {
        "development": (inner_training, seen[:, 5]),
        "seen": (seen[:, 0], seen[:, 6:].flatten()),
        "unseen": (np.append(seen[:, 0], unseen[0]), unseen[1:]),
    }
    roles = {}
    for position, label in ((0, "train_and_enroll"), (5, "development_probe")):
        roles.update(dict.fromkeys(seen[:, position], label))
    roles.update(dict.fromkeys(seen[:, 6:].flatten(), "seen_probe"))
    roles[unseen[0]] = "unseen_enroll"
    roles.update(dict.fromkeys(unseen[1:], "unseen_probe"))
    rows = [
        {
            "database": database, "finger": int(finger), "file": name,
            "training": int(index in training), "inner_training": int(index in inner_training),
            "role": roles.get(index, "train"),
        }
        for index, (name, finger) in enumerate(zip(bank.names, bank.fingers))
    ]
    return Split(int(ids[-1]), training, inner_training, pair_inputs, rows)


def pair_indices(bank, split, group):
    enroll, probes = split.pair_inputs[group]
    first, second = np.repeat(enroll, len(probes)), np.tile(probes, len(enroll))
    if np.any(first == second):
        raise ValueError("Enrollment and probe captures overlap")
    genuine = bank.fingers[first] == bank.fingers[second]
    return first, second, genuine


def scores_for(bank, pairs, condition, model, weight):
    first, second, genuine = pairs
    gallery, enroll_index = np.unique(first, return_inverse=True)
    captures, probe_index = np.unique(second, return_inverse=True)
    original = normalized_vectors(bank.images["standard", "original"][gallery], model)
    probes = normalized_vectors(bank.images["standard", condition][captures], model)
    return compare_vectors(original[enroll_index], probes[probe_index], weight), genuine


def mean_over_settings(values, groups):
    return statistics.mean(statistics.mean(values[key] for key in group) for group in groups)


def objective(banks, development, model):
    values_by_database = []
    for database, bank in banks.items():
        values = {}
        for condition in CONDITIONS:
            score, genuine = scores_for(bank, development[database], condition, model, EXPERIMENT_WEIGHT)
            values[condition] = eer(score, genuine)
        values_by_database.append((values["original"] + mean_over_settings(values, AUGMENTATION_GROUPS)) / 2)
    return statistics.mean(values_by_database)


def calibrate(banks, development, model, weight):
    impostors = {}
    for database, bank in banks.items():
        scores, genuine = scores_for(bank, development[database], "original", model, weight)
        impostors[database] = [scores[~genuine]]
    return choose_threshold(impostors)


def metrics(score, genuine, threshold):
    return {
        "genuine_pairs": int(genuine.sum()), "impostor_pairs": int((~genuine).sum()),
        "eer": eer(score, genuine), "threshold": threshold / 1000,
        "fmr": float(np.mean(score[~genuine] >= threshold)),
        "fnmr": float(np.mean(score[genuine] < threshold)),
    }


def aggregate_metrics(values, groups):
    keys = [key for group in groups for key in group]
    return {
        "augmentation_settings": len(groups),
        "image_variants": len(keys),
        "genuine_pairs": sum(values[key]["genuine_pairs"] for key in keys),
        "impostor_pairs": sum(values[key]["impostor_pairs"] for key in keys),
        **{name: mean_over_settings({key: values[key][name] for key in keys}, groups)
           for name in ("eer", "fmr", "fnmr")},
    }


def native_validate(dll, candidate, metadata, banks, splits, model):
    if candidate.name != "deepprint_texminu_512.onnx":
        raise ValueError("DLL validation needs the standard ONNX filename")
    staged = candidate.parent / dll.name
    shutil.copy2(dll, staged)
    shutil.copy2(dll.with_name("onnxruntime.dll"), candidate.parent / "onnxruntime.dll")
    native = ctypes.CDLL(str(staged.resolve()))
    args = (ctypes.c_int, ctypes.c_void_p, ctypes.c_void_p, ctypes.POINTER(ctypes.c_int),
            ctypes.POINTER(ctypes.c_int), ctypes.c_void_p)
    for function in (native.KISA_Enroll, native.KISA_Attempt):
        function.argtypes = args
        function.restype = ctypes.c_int

    roots = [Path(path) for path in metadata["manifests"]]
    if any(file_hash(root / "manifest.csv") != metadata["manifests"][str(root)] for root in roots):
        raise ValueError("An augmentation manifest changed since caching")
    paths = paths_for_conditions(Path(metadata["fvc"]), roots)
    sizes = {(300, 300): 2, (640, 480): 3}
    output = ctypes.create_string_buffer(1_000_000)

    def extract(function, path):
        with Image.open(path) as image:
            image_type = sizes[image.size]
            pixels = image.convert("L").tobytes()
        length, error = ctypes.c_int(), ctypes.c_int()
        message = ctypes.create_string_buffer(256)
        ok = function(image_type, pixels, output, ctypes.byref(length), ctypes.byref(error), message)
        template = output[:length.value]
        if ok != 1 or error.value or length.value != 2056 or template[:8] != b"DPTM\x01\x00\x00\x02":
            raise RuntimeError(f"{path}: invalid DLL template ({error.value}, {message.value!r})")
        return np.frombuffer(template, dtype="<f4", offset=8)

    rows = []
    for database, bank in banks.items():
        pairs = {group: pair_indices(bank, splits[database], group) for group in ("seen", "unseen")}
        gallery = np.unique(np.concatenate([first for first, _, _ in pairs.values()]))
        probes = np.unique(np.concatenate([second for _, second, _ in pairs.values()]))
        enrolled = np.zeros((len(bank.names), 512), dtype=np.float32)
        attempted = np.zeros_like(enrolled)
        for index in gallery:
            enrolled[index] = extract(native.KISA_Enroll, paths[database]["original"][bank.names[index]])
        for subset, conditions in NATIVE_SUBSETS.items():
            deltas = {group: 0 for group in pairs}
            for condition in conditions:
                for index in probes:
                    attempted[index] = extract(native.KISA_Attempt, paths[database][condition][bank.names[index]])
                for group, pair in pairs.items():
                    first, second, _ = pair
                    score = compare_vectors(
                        normalized_vectors(enrolled[first], None), normalized_vectors(attempted[second], None),
                        EXPERIMENT_WEIGHT,
                    )
                    expected, _ = scores_for(bank, pair, condition, model, EXPERIMENT_WEIGHT)
                    delta = int(np.max(np.abs(score - expected)))
                    if delta > 1:
                        raise ValueError(f"{database}/{group}/{condition}: DLL score parity failed ({delta} steps)")
                    deltas[group] = max(deltas[group], delta)
            for group, (first, _, _) in pairs.items():
                rows.append({"database": database, "group": group, "subset": subset,
                             "checked_pairs": len(first) * len(conditions), "max_score_delta": deltas[group] / 1000})
            print(f"DLL templates verified for {database}/{subset}", flush=True)
    return rows


def run(args):
    torch.set_num_threads(2)
    metadata = json.loads((args.cache / "source.json").read_text(encoding="utf-8"))
    if file_hash(args.model) != metadata["model_sha256"]:
        raise ValueError("The cache and source ONNX must use the same weights")
    banks = load_banks(args.cache, conditions=CONDITIONS)
    splits = {db: make_split(db, bank) for db, bank in banks.items()}
    development = {db: pair_indices(bank, splits[db], "development") for db, bank in banks.items()}
    inner_training = {db: split.inner_training for db, split in splits.items()}
    args.output.mkdir(parents=True, exist_ok=False)
    write_csv(args.output / "split.csv", [row for split in splits.values() for row in split.rows])
    baseline_objective = objective(banks, development, None)
    candidates, rows = {}, []
    for both in (False, True):
        for augmented in (False, True):
            model, epoch, score = fit(
                banks, inner_training, both, augmented, SEED, MAX_EPOCHS,
                lambda trained: objective(banks, development, trained), texture_weight=EXPERIMENT_WEIGHT,
            )
            candidates[both, augmented] = (model, epoch, score)
            row = {"heads": "both" if both else "minutiae", "augmentation": augmented, "epoch": epoch,
                   "objective": score, "baseline_objective": baseline_objective}
            rows.append(row)
            print(row, flush=True)
    write_csv(args.output / "development.csv", rows)
    best = min(candidates, key=lambda config: candidates[config][2])
    training = {db: split.training for db, split in splits.items()}
    models = {"reference_22_78": None, "baseline": None}
    thresholds = {
        "reference_22_78": calibrate(banks, development, None, TEXTURE_WEIGHT),
        "baseline": calibrate(banks, development, None, EXPERIMENT_WEIGHT),
    }
    epochs = {}
    for scorer, config in (("fine_tuned", best), ("training_ablation", (best[0], not best[1]))):
        trained, epochs[scorer], _ = candidates[config]
        thresholds[scorer] = calibrate(banks, development, trained, EXPERIMENT_WEIGHT)
        models[scorer], _, _ = fit(
            banks, training, *config, SEED, epochs[scorer], texture_weight=EXPERIMENT_WEIGHT,
        )

    results = []
    for database, bank in banks.items():
        for group in ("seen", "unseen"):
            pairs = pair_indices(bank, splits[database], group)
            for scorer, model in models.items():
                weight = TEXTURE_WEIGHT if scorer == "reference_22_78" else EXPERIMENT_WEIGHT
                threshold = thresholds[scorer]
                values = {}
                for condition in CONDITIONS:
                    score, genuine = scores_for(bank, pairs, condition, model, weight)
                    values[condition] = metrics(score, genuine, threshold)
                results.append({"database": database, "group": group, "scorer": scorer, "scope": "original",
                                "augmentation_settings": 0, "image_variants": 1, **values["original"]})
                results.append({"database": database, "group": group, "scorer": scorer, "scope": "augmentation_mean",
                                "threshold": threshold / 1000, **aggregate_metrics(values, AUGMENTATION_GROUPS)})
    write_csv(args.output / "evaluation.csv", results)

    candidate = models["fine_tuned"]
    matrices = candidate.matrices()
    np.savez(args.output / "trained_projections.npz", texture=matrices[0], minutiae=matrices[1])
    max_delta = export_head_candidate(args.model, args.candidate, matrices, banks, Path(metadata["fvc"]))
    if args.dll is not None:
        native = native_validate(args.dll, args.candidate, metadata, banks, splits, candidate)
        write_csv(args.output / "native_validation.csv", native)
    summary = {
        "protocol": "one fixed highest ID per database; six fit and two seen-test captures for each other ID",
        "held_out_ids": {db: split.held_id for db, split in splits.items()},
        "seed": SEED, "texture_weight": EXPERIMENT_WEIGHT, "thresholds": {k: v / 1000 for k, v in thresholds.items()},
        "threshold_calibration": "clean development pairs using five enroll captures and one separate probe per seen ID",
        "candidate_heads": "both" if best[0] else "minutiae", "candidate_augmented": best[1],
        "augmentation_profile": PROFILE, "augmentation_settings": len(AUGMENTATION_GROUPS),
        "augmentation_variants": len(CONDITIONS) - 1,
        "aggregation": "macro mean over settings, averaging seeds within each Gaussian setting",
        "development_objective": "equal weight to original EER and the augmentation mean EER",
        "refit_epochs": epochs, "development_baseline_objective": baseline_objective,
        "development_candidate_objective": candidates[best][2],
        "source_model_sha256": metadata["model_sha256"], "source_manifests": metadata["manifests"],
        "candidate_model_sha256": file_hash(args.candidate), "export_max_embedding_delta": max_delta,
    }
    (args.output / "summary.json").write_text(json.dumps(summary, indent=2), encoding="utf-8")
    print(json.dumps(summary, indent=2), flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("model", "cache", "output", "candidate"):
        parser.add_argument("--" + name, type=Path, required=True)
    parser.add_argument("--dll", type=Path,
                        help=f"Verify native template extraction on original and sigma-{MAX_SIGMA} samples")
    run(parser.parse_args())


if __name__ == "__main__":
    main()
