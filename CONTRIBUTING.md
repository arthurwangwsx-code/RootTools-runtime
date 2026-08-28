# Contributing

RootTools Runtime is privileged device software. Contributions should preserve the typed-capability architecture rather than adding generic privileged execution shortcuts.

## Before changing a privileged operation

Document:

1. semantic capability ID;
2. R0/R1/R2/R3 risk classification;
3. fixed Provider ownership;
4. Principal grant behavior;
5. runtime/lock prerequisites;
6. post-condition verification;
7. receipt/audit evidence;
8. recovery behavior after daemon/provider failure.

If a feature appears to require caller-controlled executable paths, argv, arbitrary shell, arbitrary Frida scripts or generic injection, redesign it as a typed semantic operation first.

## Validation

Run before opening a pull request:

```bash
bash Scripts/test.sh
git diff --check
```

For changes affecting Swift/iOS packaging, also run:

```bash
bash Scripts/build.sh
python3 Scripts/package-rootless-deb.py
```

Physical-device claims must clearly distinguish source/build validation from actual device qualification.

## Commits

Keep milestones coherent and independently revertible. Do not commit local token files, build products, DerivedData or generated Xcode projects.

## Releases

Official release artifacts are built locally with `Scripts/release-local.sh` and uploaded to GitHub Releases. Do not add hosted macOS compilation to GitHub Actions without an explicit reason.
