"""Evaluate input preprocessing and fine-tune DeepPrint's final projections."""

import argparse
import copy
import csv
from dataclasses import dataclass
import json
from pathlib import Path
import statistics
import sys

import numpy as np
import onnx
from onnx import numpy_helper
import onnxruntime as ort
import torch
from torch import nn
from torch.nn import functional as F

# The Windows embeddable interpreter omits the script directory from sys.path.
if sys.flags.isolated:
    sys.path.insert(0, str(Path(__file__).resolve().parent))

from cache_fvc_embeddings import DATABASES, PILOTS, file_hash, prepare_image, read_rows


TEXTURE_WEIGHT = 0.22
MAX_EPOCHS = 12
AUGMENTED_PROBABILITY = 0.75
NOISE_PROBABILITIES = dict(zip(DATABASES, (0.75, 0.35)))
STRESS = ("noise-10-s42", "noise-40-s42", "noise-40-s43", "rotation+10", "brightness-15")
LAYERS = (
    ("texture.6.weight", "texture.6.bias", 1536),
    ("minutia_embedding._4_linear.weight", "minutia_embedding._4_linear.bias", 1024),
)


@dataclass
class Database:
    names: np.ndarray
    fingers: np.ndarray
    development: tuple
    confirmation: tuple
    images: dict
    timings: dict


def load_banks(directory, split=None, conditions=None):
    rows = read_rows(split) if split is not None else []
    expected = set(conditions) if conditions is not None else {"original", *STRESS}
    if conditions is None:
        expected.update(f"noise-{sigma:02}-s{seed}" for sigma in (2, 5, 10, 20, 30, 40) for seed in (42, 43))
    banks = {}
    for database in DATABASES:
        memberships = {int(r["finger"]): r["split"] for r in rows if r["database"] == database}
        images, timings = {}, {}
        names = None
        for path in sorted((directory / database).glob("*/*.npz")):
            if conditions is not None and (path.parent.name != "standard" or path.stem not in expected):
                continue
            with np.load(path, allow_pickle=False) as data:
                if names is None:
                    names = data["names"]
                if not np.array_equal(names, data["names"]):
                    raise ValueError(f"Inconsistent image order in {path}")
                images[path.parent.name, path.stem] = data["embeddings"]
                timings[path.parent.name, path.stem] = data["attempt_ms"]
        if names is None:
            raise ValueError(f"No cached embeddings for {database}")
        fingers = np.array([int(name.split("_", 1)[0]) for name in names])
        development = tuple(sorted(f for f, group in memberships.items() if group == "development"))
        confirmation = tuple(sorted(f for f, group in memberships.items() if group == "confirmation"))
        if split is not None and (
            set(fingers) != set(memberships) or len(development) != 5 or len(confirmation) != 5
        ):
            raise ValueError(f"Expected five development and five confirmation IDs for {database}")
        if not expected.issubset({key for mode, key in images if mode == "standard"}):
            raise ValueError(f"Missing cached conditions for {database}")
        banks[database] = Database(names, fingers, development, confirmation, images, timings)
    return banks


class ResidualProjection(nn.Module):
    def __init__(self, rank=4):
        super().__init__()
        self.down = nn.Linear(256, rank, bias=False)
        self.up = nn.Linear(rank, 256, bias=False)
        nn.init.zeros_(self.up.weight)

    def forward(self, vectors):
        return F.normalize(vectors + self.up(self.down(vectors)), dim=-1)

    def matrix(self):
        return (torch.eye(256) + self.up.weight @ self.down.weight).detach().numpy()


class Heads(nn.Module):
    def __init__(self, both, texture_only=False):
        super().__init__()
        self.texture = ResidualProjection() if both or texture_only else nn.Identity()
        self.minutiae = nn.Identity() if texture_only else ResidualProjection()

    def forward(self, vectors):
        return torch.cat((self.texture(vectors[:, :256]), self.minutiae(vectors[:, 256:])), dim=1)

    def matrices(self):
        return tuple(branch.matrix() if isinstance(branch, ResidualProjection) else np.eye(256, dtype=np.float32)
                     for branch in (self.texture, self.minutiae))


def torch_scores(a, b, texture_weight):
    texture = (a[:, :256] * b[:, :256]).sum(dim=1)
    minutiae = (a[:, 256:] * b[:, 256:]).sum(dim=1)
    return (1.0 + texture_weight * texture + (1.0 - texture_weight) * minutiae) * 0.5


def normalized_vectors(embeddings, model):
    if model is not None:
        with torch.inference_mode():
            embeddings = model(torch.from_numpy(embeddings)).numpy()
    blocks = embeddings.astype(np.float64).reshape(-1, 2, 256)
    return blocks / np.linalg.norm(blocks, axis=2, keepdims=True)


def compare_vectors(first, second, texture_weight=TEXTURE_WEIGHT):
    cosines = np.sum(first * second, axis=2)
    similarity = (1.0 + cosines @ np.array([texture_weight, 1.0 - texture_weight])) / 2.0
    return np.floor(np.clip(similarity, 0.0, 1.0) * 1000 + 0.5).astype(int)


def pairs_for(bank, fingers, conditions, model=None, mode="standard"):
    selected = np.flatnonzero(np.isin(bank.fingers, fingers))
    i, j = np.triu_indices(len(selected), 1)
    enrolled, probe = selected[i], selected[j]
    genuine = bank.fingers[enrolled] == bank.fingers[probe]
    base = normalized_vectors(bank.images[mode, "original"][selected], model)
    for condition in conditions:
        variants = base if condition == "original" else normalized_vectors(bank.images[mode, condition][selected], model)
        yield condition, compare_vectors(base[i], variants[j]), genuine


def eer(scores, genuine):
    g = np.bincount(scores[genuine], minlength=1001)
    i = np.bincount(scores[~genuine], minlength=1001)
    observed = (g + i)[::-1] > 0
    fmr = np.r_[0.0, i[::-1].cumsum()[observed] / i.sum()]
    fnmr = np.r_[1.0, (g.sum() - g[::-1].cumsum()[observed]) / g.sum()]
    crossed = np.flatnonzero(fmr >= fnmr)[0]
    if fmr[crossed] == fnmr[crossed]:
        return float(fmr[crossed])
    return float(min(fmr[crossed - 1] + fnmr[crossed - 1], fmr[crossed] + fnmr[crossed]) / 2)


def evaluation_objective(banks, validation, model):
    clean, stress = [], []
    for database, bank in banks.items():
        for key, scores, genuine in pairs_for(bank, validation[database], ("original", *STRESS), model):
            (clean if key == "original" else stress).append(eer(scores, genuine))
    return 0.5 * statistics.mean(clean) + 0.5 * statistics.mean(stress)


class Triplets:
    def __init__(self, bank, train_indices, database, texture_weight):
        self.images = {key: v for (mode, key), v in bank.images.items() if mode == "standard"}
        self.ids = np.asarray(train_indices)
        self.noise = sorted(key for key in self.images if key.startswith("noise"))
        self.pose = sorted(key for key in self.images if key.startswith(("rotation", "brightness")))
        self.rotation = sorted(key for key in self.images if key.startswith("rotation"))
        self.noise_probability = NOISE_PROBABILITIES[database]
        self.positives, self.negatives = {}, {}
        base = normalized_vectors(self.images["original"][self.ids], None)
        fingers = bank.fingers[self.ids]
        for index, image in enumerate(self.ids):
            same = fingers == fingers[index]
            self.positives[image] = self.ids[same & (self.ids != image)]
            candidates = base[~same]
            scores = compare_vectors(np.broadcast_to(base[index], candidates.shape), candidates, texture_weight)
            self.negatives[image] = self.ids[~same][np.argsort(scores, kind="stable")[-6:]]

    def batch(self, random, augmented, size=48, augmentation_random=None):
        anchor, positive, negative = [], [], []
        transform_random = random if augmentation_random is None else augmentation_random
        for i in random.choice(self.ids, size=size):
            key = "original"
            if augmented and transform_random.random() < AUGMENTED_PROBABILITY:
                if augmented == "rotation":
                    pool = self.rotation
                elif augmented == "gaussian":
                    pool = self.noise
                else:
                    pool = self.noise if transform_random.random() < self.noise_probability else self.pose
                key = transform_random.choice(pool)
            anchor.append(self.images["original"][i])
            positive.append(self.images[key][random.choice(self.positives[i])])
            negative.append(self.images[key][random.choice(self.negatives[i])])
        return tuple(torch.from_numpy(np.stack(items)) for items in (anchor, positive, negative))


def fit(banks, training, both, augmented, seed, epochs, validate=None, texture_weight=TEXTURE_WEIGHT,
        *, texture_only=False, augmentation_seed=None, checkpoint=None):
    torch.manual_seed(seed)
    random = np.random.default_rng(seed)
    augmentation_random = np.random.default_rng(augmentation_seed) if augmentation_seed is not None else None
    model = Heads(both, texture_only)
    optimizer = torch.optim.AdamW(model.parameters(), lr=0.003, weight_decay=1e-4)
    batches = [Triplets(bank, training[database], database, texture_weight) for database, bank in banks.items()]
    best_state, best_loss, best_epoch = None, float("inf"), epochs
    for epoch in range(1, epochs + 1):
        for _ in range(4):
            combined = [item.batch(random, augmented, augmentation_random=augmentation_random) for item in batches]
            anchor, positive, negative = (torch.cat(items) for items in zip(*combined))
            a, p, n = model(torch.cat((anchor, positive, negative))).chunk(3)
            separation = F.relu(
                0.12 + torch_scores(a, n, texture_weight) - torch_scores(a, p, texture_weight)
            ).mean()
            drift = (a - anchor).square().sum(dim=1).mean()
            loss = separation + 0.05 * drift
            optimizer.zero_grad()
            loss.backward()
            optimizer.step()
        if checkpoint is not None:
            checkpoint(epoch, model)
        if validate is not None:
            result = validate(model)
            if result < best_loss - 1e-10:
                best_state, best_loss, best_epoch = copy.deepcopy(model.state_dict()), result, epoch
    if best_state is not None:
        model.load_state_dict(best_state)
    return model, best_epoch, best_loss


def metrics_row(database, scorer, mode, condition, scores, genuine, threshold):
    return {
        "database": database, "scorer": scorer, "preprocessing": mode, "condition": condition,
        "genuine_pairs": int(genuine.sum()), "impostor_pairs": int((~genuine).sum()),
        "eer": eer(scores, genuine), "threshold": threshold / 1000,
        "fmr": float(np.mean(scores[~genuine] >= threshold)),
        "fnmr": float(np.mean(scores[genuine] < threshold)),
    }


def write_csv(path, rows):
    with path.open("w", encoding="utf-8", newline="") as sink:
        writer = csv.DictWriter(sink, fieldnames=list(rows[0]))
        writer.writeheader()
        writer.writerows(rows)


def best_preprocessing(banks):
    rows, selected = [], {}
    for database, bank in banks.items():
        selected[database] = "standard"
        best = float("inf")
        for mode, conditions in (("standard", next(iter(PILOTS[database].values()))), *PILOTS[database].items()):
            scores_by_case = []
            for condition, scores, genuine in pairs_for(bank, bank.development, conditions, mode=mode):
                measured = metrics_row(database, "baseline", mode, condition, scores, genuine, 966)
                rows.append(measured)
                scores_by_case.append(measured["eer"])
            candidate = statistics.mean(scores_by_case)
            if candidate < best - 1e-10:
                best, selected[database] = candidate, mode
    return rows, selected


def original_parity(banks, path):
    previous = read_rows(path)
    checked, max_step = 0, 0
    for database, bank in banks.items():
        index = {name: i for i, name in enumerate(bank.names)}
        original = normalized_vectors(bank.images["standard", "original"], None)
        for row in previous:
            if row["database"] != database:
                continue
            i = index[Path(row["enrolled"]).name]
            j = index[Path(row["probe"]).name]
            score = compare_vectors(original[i:i + 1], original[j:j + 1])[0]
            max_step = max(max_step, abs(int(score) - round(float(row["score"]) * 1000)))
            checked += 1
    if checked != 6320 or max_step > 1:
        raise ValueError(f"Offline scoring parity failed for {checked} original pairs; maximum difference {max_step}")
    return {"verified_pairs": checked, "max_score_delta": max_step / 1000}


def choose_threshold(out_of_fold):
    thresholds = []
    for database in DATABASES:
        combined = np.concatenate(out_of_fold[database])
        ordered = np.sort(combined)[::-1]
        thresholds.append(int(ordered[len(ordered) // 100]) + 1)
    return max(thresholds)


def run_training(args):
    torch.set_num_threads(2)
    metadata = json.loads((args.cache / "source.json").read_text(encoding="utf-8"))
    if file_hash(args.model) != metadata["model_sha256"]:
        raise ValueError("The cache and the source ONNX model must use the same weights")
    banks = load_banks(args.cache, args.split)
    parity = original_parity(banks, args.reference_pairs)
    print(f"Baseline parity: {parity}", flush=True)
    args.output.mkdir(parents=True, exist_ok=False)
    write_csv(args.output / "split.csv", [row for row in read_rows(args.split) if row["database"] in banks])
    prep_results, preprocessing = best_preprocessing(banks)
    write_csv(args.output / "preprocessing_development.csv", prep_results)

    candidates = {(both, augmented): [] for both in (False, True) for augmented in (False, True)}
    baseline_folds, fold_rows = [], []
    for fold in range(5):
        validation = {
            database: (bank.development[fold], bank.development[(fold + 1) % 5]) for database, bank in banks.items()
        }
        training = {
            database: np.flatnonzero(np.isin(bank.fingers, bank.development)
                                     & ~np.isin(bank.fingers, validation[database]))
            for database, bank in banks.items()
        }
        baseline_folds.append(evaluation_objective(banks, validation, None))
        for (both, augmented), results in candidates.items():
            model, epoch, objective = fit(
                banks, training, both, augmented, 42 + fold, MAX_EPOCHS,
                lambda trained: evaluation_objective(banks, validation, trained),
            )
            results.append((model, epoch, objective, validation))
            label = "both" if both else "minutiae"
            fold_rows.append({
                "fold": fold + 1, "heads": label, "augmented_training": augmented, "best_epoch": epoch,
                "objective": objective, "baseline_objective": baseline_folds[-1],
                **{database + "_validation": "|".join(map(str, ids)) for database, ids in validation.items()},
            })
            print(f"Fold {fold + 1}, {label}, augmented={augmented}: {objective:.4f} at epoch {epoch}", flush=True)
    write_csv(args.output / "development_folds.csv", fold_rows)

    best = min(candidates, key=lambda c: statistics.mean(row[2] for row in candidates[c]))
    comparator = (best[0], not best[1])
    refit_ids = {db: np.flatnonzero(np.isin(bank.fingers, bank.development)) for db, bank in banks.items()}
    models = {"baseline": None}
    refit_epochs = {}
    oof = {scorer: {db: [] for db in banks} for scorer in ("baseline", "fine_tuned", "training_ablation")}
    for scorer, config in (("fine_tuned", best), ("training_ablation", comparator)):
        folds = candidates[config]
        refit_epochs[scorer] = int(statistics.median(row[1] for row in folds))
        models[scorer], _, _ = fit(banks, refit_ids, *config, 42, refit_epochs[scorer])
        for model, _, _, validation in folds:
            for database, bank in banks.items():
                _, score, genuine = next(pairs_for(bank, validation[database], ("original",), model))
                oof[scorer][database].append(score[~genuine])
                if scorer == "fine_tuned":
                    _, score, genuine = next(pairs_for(bank, validation[database], ("original",)))
                    oof["baseline"][database].append(score[~genuine])
    thresholds = {scorer: choose_threshold(scores) for scorer, scores in oof.items()}

    confirmation_rows = []
    for database, bank in banks.items():
        for scorer, model in models.items():
            settings = [("standard", tuple(key for mode, key in bank.images if mode == "standard"))]
            if preprocessing[database] != "standard" and scorer != "training_ablation":
                mode = preprocessing[database]
                settings.append((mode, PILOTS[database][mode]))
            for mode, conditions in settings:
                for condition, scores, genuine in pairs_for(bank, bank.confirmation, conditions, model, mode):
                    record = metrics_row(database, scorer, mode, condition, scores, genuine, thresholds[scorer])
                    if scorer == "baseline":
                        capture = np.isin(bank.fingers, bank.confirmation)
                        record["cached_attempt_p95_ms"] = float(np.percentile(bank.timings[mode, condition][capture], 95))
                    else:
                        record["cached_attempt_p95_ms"] = ""
                    confirmation_rows.append(record)
    write_csv(args.output / "confirmation.csv", confirmation_rows)

    matrices = models["fine_tuned"].matrices()
    np.savez(args.output / "trained_projections.npz", texture=matrices[0], minutiae=matrices[1])
    export_delta = export_head_candidate(args.model, args.candidate, matrices, banks, Path(metadata["fvc"]))
    summary = {
        "source_model_sha256": metadata["model_sha256"], "scope": "FVC2002 and FVC2004; confirmation IDs held out",
        "source_manifests": metadata["manifests"],
        "candidate_onnx_sha256": file_hash(args.candidate),
        "candidate_onnx_max_embedding_delta": export_delta,
        "candidate_heads": "both" if best[0] else "minutiae", "augmented_training": best[1], "refit_epochs": refit_epochs,
        "training_ablation_uses_augmentation": comparator[1],
        "training": {"optimizer": "AdamW", "lr": 0.003, "rank": 4, "triplet_margin": 0.12, "seed": 42,
                     "augmented_probe_probability": AUGMENTED_PROBABILITY, "noise_given_augmented_probe": NOISE_PROBABILITIES},
        "evaluation": {"texture_weight": TEXTURE_WEIGHT, "objective": "equal weight to clean EER and mean stress EER",
                       "stress_conditions": STRESS, "thresholds_from_out_of_fold_development": thresholds},
        "development_baseline_objective": statistics.mean(baseline_folds),
        "development_selected_objective": statistics.mean(row[2] for row in candidates[best]),
        "preprocessing_selected_in_development": preprocessing, "baseline_parity": parity,
    }
    (args.output / "summary.json").write_text(json.dumps(summary, indent=2), encoding="utf-8")
    print(json.dumps(summary, indent=2), flush=True)


def export_head_candidate(source, candidate, matrices, banks, fvc):
    if candidate.exists():
        raise ValueError(f"Candidate already exists: {candidate}")
    model = onnx.load(source)
    initializers = {item.name: item for item in model.graph.initializer}
    for (weight_name, bias_name, inputs), matrix in zip(LAYERS, matrices):
        weight = numpy_helper.to_array(initializers[weight_name])
        bias = numpy_helper.to_array(initializers[bias_name])
        if weight.shape != (256, inputs) or bias.shape != (256,):
            raise ValueError(f"Unexpected ONNX projection: {weight_name}")
        initializers[weight_name].CopyFrom(numpy_helper.from_array(matrix @ weight, weight_name))
        initializers[bias_name].CopyFrom(numpy_helper.from_array(matrix @ bias, bias_name))

    candidate.parent.mkdir(parents=True, exist_ok=True)
    temporary = candidate.with_suffix(".tmp.onnx")
    onnx.save(model, temporary)
    options = ort.SessionOptions()
    options.intra_op_num_threads = 2
    session = ort.InferenceSession(str(temporary), sess_options=options, providers=["CPUExecutionProvider"])
    max_delta = 0.0
    for database, bank in banks.items():
        expected = []
        embedding = bank.images["standard", "original"][0]
        for matrix, part in zip(matrices, np.split(embedding, 2)):
            projected = matrix @ part
            expected.append(projected / np.linalg.norm(projected))
        actual = session.run(["embedding"], {"image": prepare_image(fvc / database / bank.names[0], "standard")})[0][0]
        expected = np.concatenate(expected)
        np.testing.assert_allclose(actual, expected, atol=1e-4, rtol=1e-4)
        max_delta = max(max_delta, float(np.max(np.abs(actual - expected))))
    del session
    temporary.replace(candidate)
    print(f"Exported and verified standalone ONNX candidate: {candidate}", flush=True)
    return max_delta


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("model", "cache", "split", "reference-pairs", "output", "candidate"):
        parser.add_argument("--" + name, type=Path, required=True)
    run_training(parser.parse_args())


if __name__ == "__main__":
    main()
