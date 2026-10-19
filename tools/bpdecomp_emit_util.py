"""Small text helpers bpdecomp's body and writer share."""


def split_top(text):
    """text split at the commas outside brackets and string literals."""
    out, depth, cur, q = [], 0, '', False
    for i, ch in enumerate(text):
        if q:
            if ch == '"' and text[i - 1] != '\\': q = False
        elif ch == '"': q = True
        elif ch in '{(<[': depth += 1
        elif ch in '})>]': depth -= 1
        elif ch == ',' and depth == 0: out.append(cur.strip()); cur = ''; continue
        cur += ch
    if cur.strip(): out.append(cur.strip())
    return out
