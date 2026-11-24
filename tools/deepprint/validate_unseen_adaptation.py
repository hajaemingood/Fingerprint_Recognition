"""Validate DeepPrint adaptation on IDs excluded from training and calibration selection."""

import argparse
from dataclasses import dataclass
import json
from pathlib import Path
import statistics
import sys

import numpy as np
import torch

if sys.flags.isolated:
    sys.path.insert(0, str(Path(__file__).resolve().parent))

from cache_fvc_embeddings import PROFILE, file_hash
from finetune_fvc_heads import choose_threshold, export_head_candidate, fit, load_banks, write_csv
from finetune_last_id import (
    AUGMENTATION_GROUPS,
    CONDITIONS,
    EXPERIMENT_WEIGHT,
    SEED,
    aggregate_metrics,
    make_split,
    metrics,
    native_validate,
    pair_indices,
    scores_for,
)


FOLD_SEED = 43
STRENGTHS = (0.10, 0.25, 0.50, 1.0)
EPOCHS = (1, 3, 6, 12)
HEADS = ("texture", "both")
AUGMENTATIONS = ("original", "rotation", "gaussian")


@dataclass
class Fold:
    training: dict
    pairs: dict


class ScaledProjection:
    def __init__(self, matrices, strength):
        identity = np.eye(256, dtype=np.float32)
        self.matrices = tuple(identity + strength * (matrix - identity) for matrix in matrices)
        self.transforms = tuple(torch.from_numpy(matrix.T.copy()) for matrix in self.matrices)

    def __call__(self, features):
        return torch.cat([block @ transform for block, transform in zip(features.split(256, dim=1), self.transforms)],
                         dim=1)


def make_folds(banks, splits):
    folds = [Fold({}, {}), Fold({}, {})]
    rows = []
    for database, bank in banks.items():
        split = splits[database]
        eligible_ids = np.unique(bank.fingers[split.training])
        held_ids_by_fold = np.random.default_rng(FOLD_SEED).permutation(eligible_ids)[:4].reshape(2, 2)
        for number, (fold, held_ids) in enumerate(zip(folds, held_ids_by_fold), 1):
            held = np.isin(bank.fingers[split.training], held_ids)
            fold.training[database] = split.training[~held]
            gallery, _ = split.pair_inputs["seen"]
            probes = split.training[held & ~np.isin(split.training, gallery)]
            enroll = np.repeat(gallery, len(probes))
            probe = np.tile(probes, len(gallery))
            genuine = bank.fingers[enroll] == bank.fingers[probe]
            fold.pairs[database] = (enroll, probe, genuine)
            rows.append({
                "fold": number, "database": database,
                "training_ids": "|".join(map(str, eligible_ids[~np.isin(eligible_ids, held_ids)])),
                "validation_ids": "|".join(map(str, sorted(held_ids))),
                "training_images": len(fold.training[database]), "validation_probes": len(probes),
                "genuine_pairs": int(genuine.sum()), "impostor_pairs": int((~genuine).sum()),
            })
    return folds, rows


def evaluate(banks, pairs, model):
    return {
        database: {key: scores_for(bank, pairs[database], key, model, EXPERIMENT_WEIGHT) for key in CONDITIONS}
        for database, bank in banks.items()
    }


def evaluation_rows(scores, threshold):
    rows = []
    for database, conditions in scores.items():
        values = {key: metrics(*result, threshold) for key, result in conditions.items()}
        rows.append({"database": database, "scope": "original",
                     "augmentation_settings": 0, "image_variants": 1, **values["original"]})
        rows.append({"database": database, "scope": "augmentation_mean",
                     "threshold": threshold / 1000, **aggregate_metrics(values, AUGMENTATION_GROUPS)})
    return rows


def assess(fold_scores):
    impostors = {database: [] for database in fold_scores[0]}
    for scores in fold_scores:
        for database, conditions in scores.items():
            values, genuine = conditions["original"]
            impostors[database].append(values[~genuine])
    threshold = choose_threshold(impostors)
    rows = [{"fold": fold, **row} for fold, scores in enumerate(fold_scores, 1)
            for row in evaluation_rows(scores, threshold)]
    original = [row for row in rows if row["scope"] == "original"]
    augmented = [row for row in rows if row["scope"] == "augmentation_mean"]
    clean_eer = statistics.mean(row["eer"] for row in original)
    augmented_eer = statistics.mean(row["eer"] for row in augmented)
    return {
        "threshold": threshold / 1000, "original_eer": clean_eer, "augmentation_mean_eer": augmented_eer,
        "objective": (clean_eer + augmented_eer) / 2,
        "original_fmr": statistics.mean(row["fmr"] for row in original),
        "original_fnmr": statistics.mean(row["fnmr"] for row in original),
        "augmentation_mean_fnmr": statistics.mean(row["fnmr"] for row in augmented),
    }, rows


def fit_heads(banks, training, heads, augmentation, seed, epoch, checkpoint=None):
    return fit(
        banks, training, heads == "both", augmentation if augmentation != "original" else False,
        seed, epoch, texture_weight=EXPERIMENT_WEIGHT, texture_only=heads == "texture",
        augmentation_seed=seed + 1000, checkpoint=checkpoint,
    )[0]


def trial_grid(banks, folds, heads, augmentation, strengths, epochs):
    scores = {(strength, epoch): [] for strength in strengths for epoch in epochs}
    for number, fold in enumerate(folds, 1):
        def capture(epoch, model):
            if epoch in epochs:
                for strength in strengths:
                    projection = ScaledProjection(model.matrices(), strength)
                    scores[strength, epoch].append(evaluate(banks, fold.pairs, projection))

        fit_heads(banks, fold.training, heads, augmentation, SEED + number, max(epochs), capture)
        print(f"Development fold {number}: {heads}, {augmentation}, {max(epochs)} epochs", flush=True)

    trials = []
    for strength in strengths:
        for epoch in epochs:
            measures, rows = assess(scores[strength, epoch])
            settings = {"heads": heads, "training_augmentation": augmentation, "strength": strength, "epoch": epoch}
            trials.append(({**settings, **measures}, [{**settings, **row} for row in rows]))
    return trials


def trial_order(trial):
    return trial["objective"], trial["original_fnmr"], trial["strength"], trial["epoch"]


def run(args):
    torch.set_num_threads(2)
    metadata = json.loads((args.cache / "source.json").read_text(encoding="utf-8"))
    if file_hash(args.model) != metadata["model_sha256"]:
        raise ValueError("The cache and source ONNX must use the same weights")
    banks = load_banks(args.cache, conditions=CONDITIONS)
    splits = {db: make_split(db, bank) for db, bank in banks.items()}
    folds, fold_rows = make_folds(banks, splits)
    args.output.mkdir(parents=True, exist_ok=False)
    write_csv(args.output / "split.csv", [row for split in splits.values() for row in split.rows])
    write_csv(args.output / "development_split.csv", fold_rows)

    baseline_settings = {"heads": "frozen", "training_augmentation": "original", "strength": 0.0, "epoch": 0}
    baseline_measures, baseline_rows = assess([evaluate(banks, fold.pairs, None) for fold in folds])
    baseline = {**baseline_settings, **baseline_measures}
    grid = []
    for heads in HEADS:
        grid.extend(trial_grid(banks, folds, heads, "original", STRENGTHS, EPOCHS))
    anchor = min((trial for trial, _ in grid), key=trial_order)
    phase_three = [anchor]
    for augmentation in AUGMENTATIONS[1:]:
        trial, rows = trial_grid(
            banks, folds, anchor["heads"], augmentation, (anchor["strength"],), (anchor["epoch"],),
        )[0]
        phase_three.append(trial)
        grid.append((trial, rows))
    write_csv(args.output / "development.csv", [baseline, *(trial for trial, _ in grid)])
    write_csv(args.output / "development_by_fold.csv", [
        *({**baseline_settings, **row} for row in baseline_rows),
        *(row for _, rows in grid for row in rows),
    ])
    write_csv(args.output / "augmentation_ablation.csv", phase_three)

    eligible = [
        trial for trial, _ in grid
        if trial["objective"] < baseline["objective"] and trial["original_fnmr"] < baseline["original_fnmr"]
    ]
    selected = min(eligible or [trial for trial, _ in grid], key=trial_order)
    print(f"Selected for confirmation: {selected}; development gate passed: {bool(eligible)}", flush=True)
    trained = fit_heads(banks, {db: split.training for db, split in splits.items()}, selected["heads"],
                        selected["training_augmentation"], SEED, selected["epoch"])
    control = ScaledProjection(trained.matrices(), 1.0)
    candidate = ScaledProjection(trained.matrices(), selected["strength"])
    evaluations = [("pretrained", None, baseline), ("candidate", candidate, selected)]
    full_strength = next((trial for trial, _ in grid
                          if trial["strength"] == 1.0 and trial["heads"] == selected["heads"]
                          and trial["epoch"] == selected["epoch"]
                          and trial["training_augmentation"] == selected["training_augmentation"]), None)
    if selected["strength"] < 1.0 and full_strength is not None:
        evaluations.append(("full_strength", control, full_strength))
    results = []
    for group in ("seen", "unseen"):
        pairs = {db: pair_indices(bank, splits[db], group) for db, bank in banks.items()}
        for name, model, settings in evaluations:
            threshold = round(settings["threshold"] * 1000)
            results.extend({"group": group, "scorer": name, **row}
                           for row in evaluation_rows(evaluate(banks, pairs, model), threshold))
    write_csv(args.output / "evaluation.csv", results)

    np.savez(args.output / "trained_projections.npz", texture=candidate.matrices[0], minutiae=candidate.matrices[1])
    export_delta = export_head_candidate(args.model, args.candidate, candidate.matrices, banks, Path(metadata["fvc"]))
    if args.dll is not None:
        write_csv(args.output / "native_validation.csv",
                  native_validate(args.dll, args.candidate, metadata, banks, splits, candidate))
    summary = {
        "selected": selected, "pretrained": baseline, "augmentation_comparison_anchor": anchor,
        "development_gate_passed": bool(eligible),
        "development_gate": "both objective and original FNMR below pretrained at calibrated FMR <= 1%",
        "objective": "mean of original EER and 11-setting augmentation mean EER, equally weighted by DB and fold",
        "threshold_calibration": "lowest common threshold with original FMR <= 1% in both databases, pooled across development folds",
        "held_out_ids": {db: split.held_id for db, split in splits.items()},
        "fold_seed": FOLD_SEED, "seed": SEED, "texture_weight": EXPERIMENT_WEIGHT, "augmentation_profile": PROFILE,
        "comparison_epochs": EPOCHS, "strengths": STRENGTHS, "source_model_sha256": metadata["model_sha256"],
        "candidate_model_sha256": file_hash(args.candidate), "export_max_embedding_delta": export_delta,
    }
    (args.output / "summary.json").write_text(json.dumps(summary, indent=2), encoding="utf-8")
    print(f"Saved verification results: {args.output}", flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("model", "cache", "output", "candidate"):
        parser.add_argument("--" + name, type=Path, required=True)
    parser.add_argument("--dll", type=Path, help="Verify C++ template parity on originals and maximum noise")
    run(parser.parse_args())


if __name__ == "__main__":
    main()
