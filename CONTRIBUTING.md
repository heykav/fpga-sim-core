# Contributing

Thanks for looking. Bug reports, fixes and documentation improvements are welcome.

1. Open an issue first for anything bigger than a small fix, so we can agree on the approach.
2. Fork, make a branch, and keep the change small and focused.
3. Add or update a test that fails before your change and passes after.
4. Run the same checks as CI before opening the pull request:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

5. In the pull request, say what changed and how you checked it.

By contributing you agree your work is released under the repository's licence. Please follow the [code of conduct](CODE_OF_CONDUCT.md).
