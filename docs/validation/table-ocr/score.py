# Scores predicted grids ({stem: [[cell, ...], ...]}) against corpus/truth.json:
# a truth cell with text counts when the same row and column holds (about) the
# same text; text where the truth has none is "extra". Usage: score.py pred.json [more.json ...]
import difflib, json, os, re, sys

HERE = os.path.dirname(os.path.abspath(__file__))

def norm(s):
    s = re.sub(r"\s+", "", s or "")
    return s.replace("，", ",").replace("：", ":").replace("（", "(").replace("）", ")").replace("～", "~").replace("—", "-").lower()

def same(a, b):
    a, b = norm(a), norm(b)
    return a == b or (len(a) > 0 and difflib.SequenceMatcher(None, a, b).ratio() >= 0.75)

def trim(grid):
    grid = [list(r) for r in grid if any(norm(x) for x in r)]
    if not grid:
        return grid
    width = max(len(r) for r in grid)
    grid = [r + [""] * (width - len(r)) for r in grid]
    keep = [c for c in range(width) if any(norm(r[c]) for r in grid)]
    return [[r[c] for c in keep] for r in grid]

def score(truth, pred):
    truth, pred = trim(truth), trim(pred)
    want = [(r, c) for r, row in enumerate(truth) for c, x in enumerate(row) if norm(x)]
    hit = sum(1 for r, c in want if r < len(pred) and c < len(pred[r]) and same(truth[r][c], pred[r][c]))
    extra = sum(1 for r, row in enumerate(pred) for c, x in enumerate(row)
                if norm(x) and not (r < len(truth) and c < len(truth[r]) and norm(truth[r][c])))
    shape = len(pred) == len(truth) and (not pred or len(pred[0]) == len(truth[0]))
    return hit, len(want), extra, shape

def main():
    truth = json.load(open(os.path.join(HERE, "corpus", "truth.json"), encoding="utf-8"))
    preds = [(os.path.splitext(os.path.basename(p))[0], json.load(open(p, encoding="utf-8"))) for p in sys.argv[1:]]
    print(f'{"picture":26}' + "".join(f"{name:>22}" for name, _ in preds))
    totals = [[0, 0, 0, 0] for _ in preds]
    for stem in truth:
        line = f"{stem:26}"
        for i, (_, pred) in enumerate(preds):
            hit, want, extra, shape = score(truth[stem]["cells"], pred.get(stem) or [])
            perfect = hit == want and extra == 0 and shape
            totals[i][0] += hit; totals[i][1] += want; totals[i][2] += extra; totals[i][3] += perfect
            line += f'{("OK " if perfect else "   ") + f"{hit}/{want}" + (f" +{extra}" if extra else "") + ("" if shape else " shape"):>22}'
        print(line)
    print(f'{"cells right":26}' + "".join(f"{t[0] / t[1] * 100:>21.1f}%" for t in totals))
    print(f'{"extra cells":26}' + "".join(f"{t[2]:>22}" for t in totals))
    print(f'{"pictures all right":26}' + "".join(f"{str(t[3]) + '/' + str(len(truth)):>22}" for t in totals))

main()
