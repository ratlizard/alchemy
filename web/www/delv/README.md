# Vendored copies of grimoire's Delver format code

Four files, copied verbatim from `ratlizard/grimoire`'s `js/`:

| | |
|---|---|
| `mac-bytes.js` | big-endian readers, Mac Roman, Pascal strings |
| `mac-containers.js` | BinHex 4.0 and MacBinary, the two single-file wrappers a patch travels in |
| `mac-stuffit.js` | lists a StuffIt archive. **Not a decompressor** — it names the method and stops |
| `delv-archive.js` | the Delver Archive reader and writer, and `mergeDelverPatch` |

**Copies, not originals.** The canonical file is `js/<name>` in grimoire; fix
bugs there and re-copy. `check_copies.sh` compares each file here against the
grimoire checkout beside this repository and skips cleanly when it is absent.

It compares the **whole file**, not a recorded hash of the original the way
`tools/COPIES.txt` does, because these four run in the player rather than
beside it: a local edit that made the player behave differently from the tool
that proves the format would be exactly the failure worth catching, and a
recorded-hash check cannot see one. `COPIES.txt` records the hashes anyway, so
a checkout with no sibling still says which versions these are.

They are loaded as plain scripts, in the order the table gives, before the
page's own script; each defines globals and none is a module. Nothing else in
grimoire's `js/` is needed — that order loads clean with no other dependency,
which `check_copies.sh` does not test and the player's boot does.
