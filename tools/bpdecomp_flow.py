"""bpdecomp's control flow: a function's top-level statements into a control-flow graph with the execution-flow stack
resolved, then that graph into structured code (Ramsey, "Beyond Relooper", JFP 2022) - if, while (true) with
continue, and forward jumps out of blocks, which the C++ lowering writes as goto where no break or fall-through says
it. Every program point is a (statement, flow stack) pair, so a statement reached under two different stacks is two
nodes: the Pop at its end goes to two places."""

RET = ('RET',)      # the node every return and every Pop of an empty stack goes to

OP_RETURN, OP_JUMP, OP_JUMPIFNOT, OP_PUSH, OP_POP, OP_POPIFNOT, OP_COMPUTED, OP_END = \
    0x04, 0x06, 0x07, 0x4C, 0x4D, 0x4F, 0x4E, 0x53
NOOPS = {0x0B, 0x50, 0x5A, 0x5E, 0x6A}     # Nothing, Breakpoint, WireTracepoint, Tracepoint, InstrumentationEvent


def target(n):
    return next(v[1] for v in n.ops if v[0] == 'int')


class Graph:
    """nodes: key -> (kind, payload, succs). kind is 'stmt' (payload the statement Node), 'cond' (payload the condition;
    succs (true, false)) or 'ret'. A key is (statement index, stack), the stack a tuple of statement indexes."""
    def __init__(s, stmts, entry_mem, split=None):
        s.stmts = stmts
        s.at = {n.mem: i for i, n in enumerate(stmts)}
        s.nodes = {RET: ('ret', None, ())}
        s.split = split                     # stmt -> ('latent', resume mem) for a latent call the caller splits
        s._todo = []
        s.entry = s._goto_free((s.at[entry_mem], ()))
        s._build()

    def _key_of(s, mem, stack):
        return (s.at[mem], stack)

    def _goto_free(s, key, seen=None):
        """The first node from key that is not a pure jump, building nodes as it goes."""
        seen = seen or set()
        while True:
            if key == RET: return RET
            if key in seen: raise ValueError('a jump loop with no statement in it')
            seen.add(key)
            i, stack = key
            n = s.stmts[i]
            op = n.op
            if op == OP_JUMP: key = s._key_of(target(n), stack); continue
            if op == OP_PUSH: key = (i + 1, stack + (s.at[target(n)],)); continue
            if op == OP_POP: key = (stack[-1], stack[:-1]) if stack else RET; continue
            if op in NOOPS: key = (i + 1, stack); continue
            if op in (OP_RETURN, OP_END): return RET
            if key not in s.nodes: s._todo.append(key)
            return key

    def _build(s):
        work = s._todo
        while work:
            k = work.pop()
            if k in s.nodes or k == RET: continue
            i, stack = k
            n = s.stmts[i]
            op = n.op
            if op == OP_JUMPIFNOT:
                t = s._goto_free((i + 1, stack)); f = s._goto_free(s._key_of(target(n), stack))
                s.nodes[k] = ('cond', n.kids[0], (t, f))
            elif op == OP_POPIFNOT:
                t = s._goto_free((i + 1, stack)); f = s._goto_free((stack[-1], stack[:-1]) if stack else RET)
                s.nodes[k] = ('cond', n.kids[0], (t, f))
            elif op == OP_COMPUTED:
                raise ValueError('a computed jump outside the ubergraph entry')
            else:
                nxt = (i + 1, stack)
                if s.split and i in s.split:
                    kind, resume = s.split[i]
                    # Linkage -1: nothing is wired after the latent node's output, so nothing resumes
                    after = s._goto_free((s.at[resume], ())) if resume in s.at else RET
                    s.nodes[k] = ('latent', n, (s._goto_free(nxt), after))
                else:
                    s.nodes[k] = ('stmt', n, (s._goto_free(nxt),))


class Block:
    def __init__(s, key):
        s.key, s.stmts, s.exit, s.succs = key, [], None, []


def blocks(g):
    """Maximal straight runs of 'stmt' nodes, each ending in a goto, a cond, a latent split or a return."""
    preds = {k: 0 for k in g.nodes}
    preds[g.entry] = preds.get(g.entry, 0) + 1
    for k, (kind, _, succs) in g.nodes.items():
        for x in succs: preds[x] += 1
    out, todo, seen = {}, [g.entry], set()
    while todo:
        k = todo.pop()
        if k in seen: continue
        seen.add(k)
        b = Block(k)
        cur = k
        while True:
            kind, pay, succs = g.nodes[cur]
            if kind == 'stmt':
                b.stmts.append(pay)
                nxt = succs[0]
                if nxt != RET and preds[nxt] == 1 and g.nodes[nxt][0] == 'stmt' and nxt != g.entry:
                    cur = nxt; continue
                if nxt == RET: b.exit = ('return',)
                else: b.exit = ('goto', nxt); b.succs = [nxt]
            elif kind == 'cond':
                b.exit = ('cond', pay, succs[0], succs[1]); b.succs = [x for x in succs if x != RET]
            elif kind == 'latent':
                b.exit = ('latent', pay, succs[0], succs[1]); b.succs = [x for x in succs if x != RET]
            else:
                b.exit = ('return',)
            break
        out[k] = b
        todo.extend(b.succs)
    return out


# ---- structuring

class If:
    def __init__(s, cond, then, other): s.cond, s.then, s.other = cond, then, other
class Loop:
    def __init__(s, label, body): s.label, s.body = label, body
class Seq:
    """A block whose end a Break(label) jumps to; the code after it follows in the parent list."""
    def __init__(s, label, body): s.label, s.body = label, body
class Break:
    def __init__(s, label): s.label = label
class Continue:
    def __init__(s, label): s.label = label
class Return:
    pass
class Latent:
    """A latent call: the call, then the code its resume point runs (later), then what the flow does right away."""
    def __init__(s, call, resume, now): s.call, s.resume, s.now = call, resume, now


class Irreducible(Exception):
    pass


def structure(bs, entry):
    succs = {k: [x for x in b.succs] for k, b in bs.items()}
    # reverse postorder
    order, seen = [], set()
    def dfs(k):
        stack = [(k, iter(succs[k]))]
        seen.add(k)
        while stack:
            node, it = stack[-1]
            nxt = next((x for x in it if x not in seen), None)
            if nxt is None:
                order.append(node); stack.pop()
            else:
                seen.add(nxt); stack.append((nxt, iter(succs[nxt])))
    dfs(entry)
    order.reverse()
    rpo = {k: i for i, k in enumerate(order)}
    preds = {k: [] for k in order}
    for k in order:
        for x in succs[k]: preds[x].append(k)
    # dominators (Cooper, Harvey, Kennedy)
    idom = {entry: entry}
    changed = True
    while changed:
        changed = False
        for k in order[1:]:
            ps = [p for p in preds[k] if p in idom]
            new = ps[0]
            for p in ps[1:]:
                a, b = p, new
                while a != b:
                    while rpo[a] > rpo[b]: a = idom[a]
                    while rpo[b] > rpo[a]: b = idom[b]
                new = a
            if idom.get(k) != new: idom[k] = new; changed = True
    def dominates(a, b):
        while True:
            if a == b: return True
            if b == entry: return False
            b = idom[b]
    for k in order:
        for x in succs[k]:
            if rpo[x] <= rpo[k] and not dominates(x, k): raise Irreducible()
    children = {k: [] for k in order}
    for k in order:
        if k != entry: children[idom[k]].append(k)
    header = {k for k in order for p in preds[k] if rpo[p] >= rpo[k]}
    merge = {k for k in order if sum(1 for p in preds[k] if rpo[p] < rpo[k]) >= 2}

    def do_tree(x):
        ys = sorted([c for c in children[x] if c in merge], key=lambda c: rpo[c], reverse=True)
        code = node_within(x, ys)
        return [Loop(x, code)] if x in header else code

    def node_within(x, ys):
        if ys:
            return [Seq(ys[0], node_within(x, ys[1:]))] + do_tree(ys[0])
        b = bs[x]
        out = [('stmt', n) for n in b.stmts]
        e = b.exit
        if e[0] == 'goto': out += branch(x, e[1])
        elif e[0] == 'cond': out.append(If(e[1], branch(x, e[2]), branch(x, e[3])))
        elif e[0] == 'latent': out.append(Latent(e[1], branch(x, e[3]), branch(x, e[2])))
        else: out.append(Return())
        return out

    def branch(x, y):
        if y == RET: return [Return()]
        if rpo[y] <= rpo[x]: return [Continue(y)]
        if y in merge: return [Break(y)]
        return do_tree(y)

    return do_tree(entry)


def linear(bs, entry):
    """The fallback for a graph structure() refuses: every block in order behind its label, every edge a goto."""
    out = []
    for k, b in bs.items():
        body = [('stmt', n) for n in b.stmts]
        e = b.exit
        if e[0] == 'goto': body.append(Break(e[1]))
        elif e[0] == 'cond': body.append(If(e[1], [Break(e[2]) if e[2] != RET else Return()],
                                            [Break(e[3]) if e[3] != RET else Return()]))
        elif e[0] == 'latent': body.append(Latent(e[1], [Break(e[3]) if e[3] != RET else Return()],
                                                  [Break(e[2]) if e[2] != RET else Return()]))
        else: body.append(Return())
        out.append((k, body))
    return out
