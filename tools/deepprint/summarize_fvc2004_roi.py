"""Compare two native FVC2004 evaluations using the same pairs and split."""

import argparse
import json
from pathlib import Path
import sys

import numpy as np

if sys.flags.isolated:
    sys.path.insert(0, str(Path(__file__).resolve().parent))

from cache_fvc_embeddings import PROFILE, condition_key, file_hash, read_rows
from finetune_fvc_heads import write_csv
from finetune_last_id import AUGMENTATION_GROUPS, CONDITIONS, aggregate_metrics, metrics


DATABASE = "FVC2004_DB1_B"


def load_scores(path):
    pairs = {condition: {} for condition in CONDITIONS}
    for row in read_rows(path):
        if row["database"] != DATABASE or row["status"] != "ok":
            raise ValueError(f"Unexpected database or failed native comparison in {path}")
        condition = condition_key(row)
        pair = (Path(row["enrolled"]).name, Path(row["probe_original"]).name)
        if pair in pairs[condition]:
            raise ValueError(f"Duplicate pair in {path}: {condition}/{pair}")
        pairs[condition][pair] = (int(row["genuine"]), round(float(row["score"]) * 1000))
    return pairs


def split_pairs(args, ordered):
    memberships = {
        int(row["finger"]): row["split"] for row in read_rows(args.split) if row["database"] == DATABASE
    }
    final_rows = [row for row in read_rows(args.last_id_split) if row["database"] == DATABASE]
    gallery = {row["file"] for row in final_rows if row["role"] in ("train_and_enroll", "unseen_enroll")}
    last_probes = {row["file"] for row in final_rows if row["role"] == "unseen_probe"}
    groups = {"all": [], "development": [], "confirmation": [], "last_id": []}
    for index, (enrolled, probe) in enumerate(ordered):
        first, second = (memberships[int(name.split("_", 1)[0])] for name in (enrolled, probe))
        groups["all"].append(index)
        if first == second:
            groups[first].append(index)
        if enrolled in gallery and probe in last_probes:
            groups["last_id"].append(index)
    expected = {"all": 3160, "development": 780, "confirmation": 780, "last_id": 70}
    if {name: len(indices) for name, indices in groups.items()} != expected:
        raise ValueError("The native pair list does not match the two held-out protocols")
    return groups


def threshold_at_one_percent(scores, genuine):
    impostors = np.sort(scores[~genuine])[::-1]
    return int(impostors[len(impostors) // 100]) + 1


def summarize(args):
    datasets = {"standard": load_scores(args.standard_pairs), "roi": load_scores(args.roi_pairs)}
    reference = datasets["standard"]["original"]
    ordered = list(reference)
    genuine = np.array([label == 1 for label, _ in reference.values()])
    groups = split_pairs(args, ordered)
    rows, operating_checks, thresholds = [], [], {}
    for model, conditions in datasets.items():
        if any(pairs.keys() != reference.keys() for pairs in conditions.values()):
            raise ValueError(f"Missing or mismatched pairs for {model}")
        labels_and_scores = {key: np.array([pairs[pair] for pair in ordered]) for key, pairs in conditions.items()}
        if any(not np.array_equal(values[:, 0], genuine) for values in labels_and_scores.values()):
            raise ValueError(f"Pair labels differ across the evaluated conditions for {model}")
        original = labels_and_scores["original"][:, 1]
        development = groups["development"]
        threshold = threshold_at_one_percent(original[development], genuine[development])
        thresholds[model] = threshold / 1000
        for group, selected in groups.items():
            values = {
                key: metrics(scores[selected, 1], genuine[selected], threshold)
                for key, scores in labels_and_scores.items()
            }
            prefix = {"database": DATABASE, "scorer": model, "group": group}
            rows.append({**prefix, "scope": "original",
                         "augmentation_settings": 0, "image_variants": 1, **values["original"]})
            rows.append({**prefix, "scope": "augmentation_mean",
                         "threshold": threshold / 1000, **aggregate_metrics(values, AUGMENTATION_GROUPS)})
            if group in ("confirmation", "last_id"):
                actual = original[selected]
                genuine_group = genuine[selected]
                diagnostic = threshold_at_one_percent(actual, genuine_group)
                for mode, cutoff in (("standard_development_threshold", round(thresholds["standard"] * 1000)),
                                     ("observed_fmr_cap", diagnostic)):
                    operating_checks.append({**prefix, "mode": mode, **metrics(actual, genuine_group, cutoff)})

    args.output.mkdir(parents=True, exist_ok=False)
    write_csv(args.output / "evaluation.csv", rows)
    write_csv(args.output / "low_fmr_diagnostic.csv", operating_checks)
    metadata = {
        "pair_files_sha256": {str(path): file_hash(path) for path in (args.standard_pairs, args.roi_pairs)},
        "augmentation_profile": PROFILE,
        "aggregation": "mean across 11 settings, with both seeds averaged within each Gaussian setting",
        "thresholds": thresholds,
        "threshold_selection": "lowest threshold producing FMR at most 1% on original development pairs",
        "observed_fmr_cap": "descriptive point from original evaluation scores, separate from threshold selection",
        "texture_weight": 0.22,
        "native_comparisons_per_dll": len(ordered) * len(CONDITIONS),
        "native_pair_failures": 0,
    }
    (args.output / "summary.json").write_text(json.dumps(metadata, indent=2), encoding="utf-8")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("standard-pairs", "roi-pairs", "split", "last-id-split", "output"):
        parser.add_argument("--" + name, type=Path, required=True)
    summarize(parser.parse_args())


if __name__ == "__main__":
    main()
