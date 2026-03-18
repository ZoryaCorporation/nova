# Contributing to Nova

Welcome and thank you for your interest in Nova! The project is currently in **beta** and under active development by myself. I am thrilled to be building a community around it and genuinely value feedback and contributions of all kinds. However the current goal as of version 0.2.0 is to build a core team of collaborators who are aligned on the project's vision and standards, and to maintain a stable, high-quality codebase as we iterate towards a 1.0 release. With that in mind, this document may be subject to change as the project evolves.

As of this writing (v0.2.0) the most meaningful contributions from members of the community would simply be adopting and using Nova, learning the syntax, features, and its robust standard library.

If you have experience with **freestanding C projects, virtual machines, compilers, or bytecode formats** and are interested in contributing code, the best way to get started is to get in touch with me directly through GitHub Discussions or email. I can provide guidance on the codebase, the contribution process, and how to get involved in a way that aligns with the project's current needs and roadmap.

## Contribution Philosophy

Nova is a register-based bytecode virtual machine with a custom compiler,
garbage collector, string interning pool, and hash table implementation — all
written from scratch to exacting standards. Making changes to these systems
requires deep familiarity with the codebase and its invariants. A well-meaning
patch to the GC, the instruction set, or the value representation can
introduce subtle bugs that don't surface until much later.

**This means we operate as a curated project:**

- **Pull requests are treated as proposals**, not guaranteed merges.
- Core systems (VM dispatch loop, GC, compiler, bytecode format, NaN-boxing,
  upvalue/closure model) are maintained exclusively by the core team.
- Community contributions are most impactful in areas like standard library
  modules, documentation, tests, examples, and clearly-scoped bug fixes.
- We will always explain why a PR isn't merged. "Not right now" is not a
  rejection of you — it's a commitment to stability.

This isn't gatekeeping for its own sake. It's how we ensure Nova remains
correct, fast, and consistent as it grows. We're looking to build a team of
trusted collaborators over time, and the path there is through engagement,
discussion, and demonstrated understanding of the project. If you are serious
about contributing and working with the Nova project, reach out to me and
let's collaborate.

I also would like to emphasize that **using Nova and providing feedback is the most meaningful contribution to me personally.** Even if you don't have the time to contribute code, just trying out the language, writing programs, sharing your use cases, ideas and feedback is incredibly valuable. It helps me understand how people are using Nova, what features are most important, and where the pain points are. So please don't hesitate to engage in discussions, open issues, or share your thoughts — it's all part of building a great community around Nova.

## Community Support & Resources
Nova is a new project and the current focus is on building the community and the core team. As up to date, Nova has been a one-person project. To address this, We will set up an offical Nova Discord server in the near future to provide a more interactive and inclusive space for the community to connect, ask questions, share ideas, and collaborate.

In addition to the Discord server we will have a dedicated website with comprehensive documentation, guides, deep dives into the internals of the VM, compiler, GC, and more. Between the website, GitHub Discussions, and the Discord server, there will be multiple channels for support, learning, and community engagement.

## Funding & Sponsorship
This topic is of course important to address and also share my ethical stance on. Nova, it's development and all associated work is currently funded by myself. **Nova is not a simple passion project that will be abandoned, or disappear.** For the sake of complete transparency, Nova's development has not been funded by or for any external entities, corporations, or organizations. I have no additional motives beyond building a great language, engineering solutions and the community around it. If you are interested in supporting the project financially, GitHub Sponsors is the most impactful way to do so. It helps fund development, infrastructure, and would allow me to invest my full time into the project. I am committed to keeping Nova open source and free to use, and any funding received will be used to accelerate development and improve the project for everyone.

Given Nova's current stage of development, resources for testing on a wide range of platforms, architectures, and environments are in high demand. To ensure that nova is completely freestanding and is rigirously tested across operating systems, different CPU architectures, and other hardware systems I would need access to a massive range of testing environments. This is something that is something that cannot be funded by myself alone, and is a critical area where improvements and validation are needed. This will be an ongoing effort and I welcome any support or contributions in this area.



## Getting Started

```bash
git clone https://github.com/zorya-corporation/nova.git
cd nova
make            # Build release
make test       # Run full test suite (28 suites, 1150+ assertions)
make DEBUG=1    # Build with AddressSanitizer + UBSan (recommended for dev)
```

**Requirements**: A C99 compiler (GCC or Clang) and `make`. That's it.
No external dependencies needed (`make NOVA_NO_NET=1` if you don't have libcurl).

## Where You Can Help Right Now

These are the highest-value, most accessible areas for community involvement:

| Area | Examples |
|------|----------|
| **Bug reports** | Crash reproducers, incorrect output, edge cases in the standard library |
| **Tests** | Expanding coverage in `tests/`, and documenting results |
| **Documentation** | Improving `nova_syntax/`, adding examples, fixing typos |
| **Example programs** | Real-world `.n` scripts in `examples/` |
| **Standard library** | Well-scoped additions to `nova_lib_*.c` modules |
| **Discussions** | Sharing use cases, ideas, and feedback in GitHub Discussions |

## What We Ask You Not To Do

To keep the project stable and the commit history clean:

- **Don't open PRs for unsolicited refactors** of internal systems, even if
  you see style inconsistencies. Consistency within the existing codebase is
  more valuable than chasing perfection.
- **Don't submit patches to the VM, GC, compiler, or bytecode format** without
  first opening an issue and discussing the change with the core team.
- **Don't fork Nova to patch syntax quirks or personal preferences.** If
  something feels wrong, open an issue — it might be a real bug or a design
  worth discussing.

## Coding Standards

Nova follows the **ZORYA-C v2.0.0** coding standard — a pragmatic, MISRA-C-inspired
standard designed for high-performance systems programming. The full standard is
published at [docs/ZORYA_C_STANDARD.md](docs/ZORYA_C_STANDARD.md).

### The Short Version

1. **C99 strict mode**: `-Wall -Wextra -Werror -pedantic -Wconversion -Wshadow`
2. **NULL checks** before every pointer dereference
3. **Allocation checks** — every `malloc`/`calloc` must be checked
4. **Explicit casts** for all type conversions
5. **Default case** in every `switch` statement
6. **`nova_` prefix** for public functions, `novai_` for internal (static) functions
7. **State pointer named `N`** (the `NovaVM *N` convention)
8. **K&R brace style**, 4-space indentation, no tabs
9. **File headers** with `@file`, `@brief`, `@author`, `@date`, `@copyright`

### Before Submitting

```bash
make clean && make           # Must compile with zero warnings
make DEBUG=1 && make test    # Must pass all tests under sanitizers
```

## How to Contribute

### Bug Reports

Open an issue using the **Bug Report** template. Include:
- Nova version (`nova --version`)
- Platform (OS, compiler, architecture)
- Minimal reproducing `.n` script
- Expected vs. actual behavior

This is genuinely the most valuable thing you can do. A clear, minimal
reproducer dramatically accelerates the fix.

### Feature Requests

Open an issue using the **Feature Request** template. Describe:
- The problem you're trying to solve
- Your proposed solution
- Alternatives you've considered

Feature requests are reviewed against the project roadmap. Not every good idea
will be accepted immediately — timing and scope matter. Our current focus is on stability and core features. Once we are out of the weeds of the intial launch and have a stable 1.0 release, we will be in a much better position to evaluate and accept new features if they align.

I can speak for myself when I say that I have a long list of features and improvements I want to make to Nova, but they cannot be rushed. I want to make sure that every change is well-designed, well-implemented, and doesn't introduce instability. So while I am always open to hearing new ideas and feature requests, the best way to contribute in this area right now is to share your thoughts in discussions and issues — it helps me understand what the community wants and how it fits within my internal development roadmap.

### Pull Requests

Before opening a PR for anything beyond documentation or tests, **open an issue
first** and discuss the change. This saves everyone time and avoids work that
can't be merged.

For approved changes:

1. **Fork** the repository
2. **Create a branch** from `main` (`git checkout -b fix/my-fix`)
3. **Make your changes** following the coding standards above
4. **Add tests** for any new behavior
5. **Run the full test suite**: `make clean && make DEBUG=1 && make test`
6. **Open a PR** referencing the issue it addresses

#### PR Checklist

- [ ] Compiles clean with `make` (zero warnings)
- [ ] Compiles clean with `make DEBUG=1` (sanitizers enabled)
- [ ] All tests pass (`make test`)
- [ ] New functionality has test coverage
- [ ] Commit messages are clear and descriptive
- [ ] Code follows ZORYA-C v2.0.0 standards
- [ ] Discussed with the team in a linked issue before implementation

### Good First Issues

Look for issues labeled [`good first issue`](https://github.com/zorya-corporation/nova/labels/good%20first%20issue).
These are specifically chosen to be approachable without deep internals knowledge.

### Documentation

Improvements to the docs, guides, and tutorials are also an ongoing process We will be expanding on these as Nova grows. Nova's language guide lives in
`nova_syntax/` — these tutorials and example programs are provided to help everyone learn the basics up to the more complex side of Nova's offerings.

## Project Structure

```
src/                    C source files (VM, compiler, standard library)
include/nova/           Public headers
include/zorya/          Vendored Zorya SDK headers
tests/                  Test suites (test_*.n files)
examples/               Example Nova programs
nova_syntax/            Language guide and tutorials
docs/                   Design documents (Blueprints, spec sheets, etc.)
```

## Tests

Tests are `.n` (Nova script) files in the `tests/` directory. Each test is
self-validating — it prints `PASS` or `FAIL` for each assertion. The test
runner (`make test`) executes all `tests/test_*.n` files and exits on first
failure.

To add a test:
1. Create or edit a `tests/test_<feature>.n` file
2. Use `assert()` or manual `echo "PASS"` / `echo "FAIL"` patterns
3. Run `make test` to verify

## Communication

- **GitHub Discussions**: Questions, ideas, and general conversation — the
  preferred place for open-ended topics
- **GitHub Issues**: Bug reports and feature requests only
- **Pull Requests**: Code contributions (please discuss first for non-trivial changes)
- **Discord**: (Coming soon!)
## License

By contributing to Nova, you agree that your contributions will be licensed
under the MIT License (see [LICENSE](LICENSE)).

---

*ZORYA CORPORATION — Engineering Excellence, Democratized*
