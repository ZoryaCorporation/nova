# Security Policy

## Supported Versions

The following versions of Nova are currently receiving security updates:

| Version | Supported          |
| ------- | ------------------ |
| latest  | :white_check_mark: |

## Reporting a Vulnerability

**Please do not report security vulnerabilities through public GitHub issues.**

If you discover a security vulnerability in Nova, please report it responsibly
through one of the following channels:

- **GitHub Private Advisory**: Use the
  [GitHub Security Advisory](https://github.com/ZoryaCorporation/nova/security/advisories/new)
  feature to report vulnerabilities privately.
- **Email**: Send details to the Zorya Corporation security team. Contact
  information is available through the
  [Zorya Corporation GitHub organization](https://github.com/ZoryaCorporation).

### What to Include

Please include as much of the following information as possible to help us
understand and reproduce the issue:

- Type of vulnerability (e.g., buffer overflow, memory corruption, injection)
- The Nova version affected
- Full path of the source file(s) related to the vulnerability
- Step-by-step instructions to reproduce the issue
- A minimal Nova script (`.n` file) that demonstrates the vulnerability
- Proof-of-concept or exploit code (if available)
- Impact assessment — what an attacker could achieve

### Response Timeline

- **Acknowledgement**: Within 72 hours of submission
- **Initial assessment**: Within 7 days
- **Resolution target**: Within 90 days for critical issues

We will keep you informed of progress throughout the process and credit you in
the release notes unless you prefer to remain anonymous.

## Security Considerations

Nova executes arbitrary scripts and has access to the filesystem, OS commands,
network, and SQLite databases via its standard library. When embedding Nova in
an application or exposing it to untrusted input, consider:

- Sandboxing the Nova process with OS-level controls (e.g., seccomp, namespaces)
- Restricting which standard library modules are available to untrusted scripts
- Validating and sanitizing any data passed into Nova scripts from external sources
- Running Nova with the minimum required filesystem and network permissions
