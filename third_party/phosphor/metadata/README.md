# Phosphor icon metadata

`regular.css` is a verbatim copy of `src/regular/style.css` from
[`@phosphor-icons/web` v2.1.2](https://github.com/phosphor-icons/web/tree/v2.1.2),
obtained from jsDelivr. It is the only source of the icon **names**: both
bundled TTFs have a version 3.0 `post` table, which contains no glyph names.

The CSS is vendored so the build never needs the network and the name-to-code
point mapping is pinned to the exact font version in this directory. It is
distributed under the same MIT license as the fonts, in `../LICENSE`.

`tools/gen_phosphor_icons.py` reads this file, checks every code point against
the cmap of both TTFs, and generates `src/app/PhosphorIcons.generated.h`:

```
python tools/gen_phosphor_icons.py           # regenerate the header
python tools/gen_phosphor_icons.py --check  # verify header, aliases, fonts
```

The generator also verifies `src/app/PhosphorAliases.h`: every Segoe alias
must name a real icon present in both fonts. Run `--check` in CI and after
editing either table. Do not edit the generated header by hand.

`--check` cannot tell that the alias table is **complete**. Upstream adds
Segoe-to-Phosphor mappings to the icon table over time, and rebasing this
branch onto a new upstream release can silently drop them when a conflict is
resolved in favour of the rewritten file. After every rebase, compare the
alias count and the Segoe code points with upstream's icon table, then run
`--check`. `tst_appearance::phosphorIcons` pins the filter keys from #279 as
a regression test.
