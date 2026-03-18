---
COMPANY: ZORYA_CORPORATION
DIVISION: PRESS OUTREACH
FILE: [Nova_DevRel.md]
AUTHOR: ANTHONY TALIENTO
DATE: 02-19-26
PUBLIC: [INTERNAL RESOURCE, DNR]
TAGS: [Nova, press, outreach, release, DevRel, PL community, open source]

---

# Nova DevRel — Outreach Network

> **Strategy**: Build genuine relationships with individuals first, scale to
> organizations second. Lead with admiration for their work, show Nova as a
> kindred project, invite them to explore it. Every email should feel personal —
> not a press blast.

> **Assets**: nova.dev (website, literature, downloads), GitHub repo (public),
> `nova --version` / `nova --help` (polished CLI), architecture docs

---

## Outreach Waves

    WAVE 1  —  Individuals (PL / VM / Systems community)
                Personal emails, reference their work, show Nova.
                Goal: build bridges, get feedback, earn early advocates.

    WAVE 2  —  Launch Day (nova.dev live + GitHub public)
                Show HN, r/ProgrammingLanguages, Lua mailing list.
                Goal: volume awareness, community discussion, stars.

    WAVE 3  —  Organizations & Enterprise (after traction)
                Redis, universities, Microsoft DevRel, Linux Foundation.
                Goal: adoption, integrations, institutional credibility.

---
---

## WAVE 1 — Key Individuals

> These are people who will genuinely understand and appreciate what Nova is.
> They live in the VM/PL/systems space. Reach out individually, reference
> their specific work, and explain what Nova does differently.

|---------------------------------------------------------------------------------|
1. `Bob Nystrom`
Role: Author of *Crafting Interpreters*, Software Engineer at Google
Email: robert@stuffwithstuff.com
Website: https://stuffwithstuff.com
GitHub: https://github.com/munificent

`BRIEF`
    Bob literally wrote the book on building bytecode VMs — *Crafting
    Interpreters* is the definitive reference and inspired countless
    projects including Nova. He deeply understands NaN-boxing, stack vs
    register VMs, garbage collection, and closure implementation. Nova's
    architecture decisions (register-based, computed-goto, tri-color GC)
    are the kind of engineering he celebrates. A genuine conversation
    about design tradeoffs would resonate. He's known for being
    approachable and responsive.

`ANGLE`
    "Your book was foundational to Nova's design. Here's where we diverged
    — register-based over stack-based, NaN-boxing with integer support,
    built-in CLI tools as first-class citizens. Would love your thoughts."

|---------------------------------------------------------------------------------|
2. `Thorsten Ball`
Role: Author of *Writing an Interpreter in Go* and *Writing a Compiler in Go*
Email: me@thorstenball.com
Website: https://thorstenball.com
GitHub: https://github.com/mrnugget
Newsletter: https://registerspill.thorstenball.com

`BRIEF`
    Thorsten is deeply embedded in the PL implementation community. His
    two books walk developers through building interpreters and compilers
    from scratch, and his "Register Spill" newsletter covers systems
    programming and language tooling. He works at Zed (the editor) and
    actively engages with people building interesting tools. Nova's
    compiler pipeline (lex → parse → AST → codegen → optimize → VM)
    and its C99 implementation would genuinely interest him.

`ANGLE`
    "Nova is a register-based VM in C99 with a full compiler pipeline —
    the kind of project your books inspired people to build. Here's what
    the architecture looks like at 15K lines."

|---------------------------------------------------------------------------------|
3. `Justine Tunney`
Role: Creator of Cosmopolitan Libc, redbean, llamafile
Email: jtunney@gmail.com
Website: https://justine.lol
GitHub: https://github.com/jart

`BRIEF`
    Justine is one of the most respected systems programmers working
    today. Her projects (Cosmopolitan Libc, redbean, llamafile) share
    Nova's philosophy: single-binary, zero-dependency, high-craft C code
    that does more with less. She appreciates code that's fast, portable,
    and elegant at the systems level. Nova's C99 implementation, NaN-boxing,
    computed-goto dispatch, and "one binary with built-in tools" story
    would resonate with her engineering sensibility.

`ANGLE`
    "Nova is a single-binary VM + toolchain in strict C99 — NaN-boxed
    values, computed-goto dispatch, built-in grep/find/cat with zero
    subprocesses. Your work on Cosmopolitan inspired the 'one binary,
    everything included' philosophy."

|---------------------------------------------------------------------------------|
4. `Roberto Ierusalimschy`
Role: Lead designer of the Lua programming language, Professor at PUC-Rio
Email: roberto@inf.puc-rio.br
Website: https://www.lua.org/authors.html
Publications: https://www.inf.puc-rio.br/~roberto/

`BRIEF`
    Roberto created Lua and its register-based VM — the direct ancestor
    of Nova's execution model. He co-authored the landmark paper "The
    Implementation of Lua 5.0" which introduced the register-based
    bytecode design that Nova builds upon. As an academic, he appreciates
    well-documented design decisions. Reaching him is reaching the source.
    He's a professor so email is his primary communication channel.

`ANGLE`
    "Nova's register-based VM is a direct descendant of Lua's design.
    We've extended the model with 0-indexed tables, built-in data
    processing (JSON/CSV/NINI), and an integrated CLI tool system.
    Your paper on Lua 5.0's implementation was essential reading."

|---------------------------------------------------------------------------------|
5. `Mike Pall`
Role: Creator of LuaJIT
Website: http://luajit.org
GitHub: https://github.com/LuaJIT/LuaJIT

`BRIEF`
    Mike built LuaJIT, one of the fastest dynamic language runtimes ever
    created. He's the world's foremost expert on NaN-boxing, trace
    compilation, and squeezing performance out of dynamic languages. He's
    notoriously selective about what he engages with, but Nova's NaN-boxing
    implementation and performance-conscious design (computed-goto, Dagger
    hash tables, Weave interning) speak his language. Even a brief
    acknowledgment from him carries enormous weight in the PL community.
    Reachable via the Lua mailing list or LuaJIT GitHub issues.

`ANGLE`
    "Nova uses NaN-boxing with integer tagging and computed-goto dispatch
    in C99 — heavily influenced by LuaJIT's approach to value
    representation. Would welcome any critique on the implementation."

|---------------------------------------------------------------------------------|
6. `Andy Wingo`
Role: Compiler engineer, co-maintainer of GNU Guile, WebAssembly contributor
Blog: https://wingolog.org
Email: wingo@pobox.com
GitHub: https://github.com/wingo

`BRIEF`
    Andy is a veteran VM implementer who has written extensively about
    garbage collection strategies, closure implementation, and bytecode
    VM design on his blog (wingolog.org). He maintains GNU Guile's
    compiler and VM and contributes to WebAssembly. His deep writing on
    GC (tri-color marking, incremental collection) directly overlaps with
    Nova's implementation choices. He engages thoughtfully with other
    VM projects.

`ANGLE`
    "Your blog posts on GC implementation and VM dispatch were reference
    material for Nova's tri-color mark-sweep collector. Here's how we
    handle incremental collection with NaN-boxed values."

|---------------------------------------------------------------------------------|
7. `Andrew Kelley`
Role: Creator of the Zig programming language
Email: andrew@ziglang.org
Website: https://ziglang.org
GitHub: https://github.com/andrewrk

`BRIEF`
    Andrew built Zig as a better systems language, and he deeply
    appreciates clean, principled C code. While Zig and Nova occupy
    different niches, he's influential in the systems programming
    community and his audience overlaps heavily with people who'd find
    Nova interesting. He's active on social media and his community
    Discord. Reaching him is more about getting Nova in front of the
    Zig community than a direct collaboration.

`ANGLE`
    "Nova is a register-based VM written in strict C99 (-Wall -Wextra
    -Werror -pedantic) — built with the same kind of engineering
    discipline Zig advocates for. Thought it might interest your community."

|---------------------------------------------------------------------------------|
8. `Chris Lattner`
Role: Creator of LLVM, Clang, Swift, and Mojo
Website: https://nondot.org/sabre/
GitHub: https://github.com/lattner

`BRIEF`
    Chris is arguably the most influential compiler engineer alive. He
    created LLVM, Clang, Swift, and now Mojo. While he's a big-name
    target, he actively engages on forums (Mojo Discord, Swift forums)
    and has historically been generous with feedback on well-crafted
    language projects. Nova's compiler pipeline and optimization passes
    are the kind of engineering he'd evaluate critically but fairly.
    Reach via public forums or Mojo community channels rather than
    cold email.

`ANGLE`
    Best reached through a Mojo community post or LLVM discourse thread
    showcasing Nova's codegen and optimization approach. Not a cold
    email target.

|---------------------------------------------------------------------------------|
9. `Brendan Eich`
Role: Creator of JavaScript, co-founder of Brave
Website: https://brendaneich.com
GitHub: https://github.com/nicedot

`BRIEF`
    Brendan created JavaScript in 10 days and understands the tradeoffs
    of dynamic language design better than almost anyone. He's been vocal
    about language design decisions, VM implementation strategies, and
    the evolution of scripting languages. Nova's approach — taking Lua's
    syntax, adding 0-indexed tables, string interpolation, built-in data
    formats — is exactly the kind of "practical language design" discussion
    he engages with on social media. Reachable via Twitter/X (@BrendanEich).

`ANGLE`
    Social media engagement first. Comment on his posts about language
    design, then introduce Nova as a "what if we redesigned a scripting
    language from scratch in 2026" conversation.

|---------------------------------------------------------------------------------|
10. `Antirez` (Salvatore Sanfilippo)
Role: Creator of Redis, author of *Redis in Action*
Email: antirez@gmail.com
Blog: http://antirez.com
GitHub: https://github.com/antirez

`BRIEF`
    Antirez created Redis and wrote its embedded Lua scripting engine.
    He's now working on other projects but remains deeply interested in
    small, elegant C codebases and scripting language design. He wrote
    a Tcl interpreter, a Lisp interpreter, and several other language
    experiments — he genuinely enjoys this space. Nova's Lua-compatible
    syntax with improved ergonomics (0-indexed, string interpolation,
    built-in data processing) could interest him as a Redis scripting
    alternative perspective. He's very responsive to thoughtful emails.

`ANGLE`
    "You embedded Lua in Redis for its simplicity and speed. Nova takes
    that same foundation — register-based VM, NaN-boxing, lightweight
    closures — and extends it with 0-indexed tables, built-in JSON/CSV
    processing, and an integrated tool system. Thought you'd appreciate
    seeing what a 'Lua reimagined' looks like in 2026."

---
---

## WAVE 2 — Community Platforms (Launch Day)

> These are high-visibility channels to hit simultaneously when nova.dev
> goes live and the GitHub repo is public. Timing matters — post in the
> morning (US Eastern), have the website and README polished, and be ready
> to engage in comments for 48 hours.

|---------------------------------------------------------------------------------|
1. `Hacker News — Show HN`
URL: https://news.ycombinator.com
How: Submit as "Show HN: Nova — A register-based bytecode VM in C99"

`BRIEF`
    The #1 launchpad for developer tools. A well-crafted Show HN post
    with a strong title and a concise top-level comment explaining the
    architecture can generate thousands of views, hundreds of GitHub
    stars, and genuine technical discussion. The HN audience loves C,
    loves VMs, and loves well-documented projects. Lead with architecture,
    not syntax.

`POST TITLE`
    "Show HN: Nova — A register-based VM with NaN-boxing, built-in
    tools, and Lua-like syntax in 15K lines of C99"

|---------------------------------------------------------------------------------|
2. `r/ProgrammingLanguages` (Reddit)
URL: https://reddit.com/r/ProgrammingLanguages
How: "Show & Tell" flair post

`BRIEF`
    80K+ subscribers who specifically care about language design and
    implementation. This is the most targeted audience on the internet
    for Nova. They'll ask about design decisions (why register-based?
    why 0-indexed? why NaN-boxing over tagged unions?), and those
    conversations become free marketing. Cross-post to r/Compilers.

|---------------------------------------------------------------------------------|
3. `Lua Mailing List`
URL: http://www.lua.org/lua-l.html
Address: lua-l@lists.lua.org

`BRIEF`
    The official Lua community. Nova's Lua-derived syntax means this
    community will immediately understand what Nova is. They'll be
    interested in the divergences (0-indexed tables, string
    interpolation, built-in data processing, tool system). Some may
    be critical — be prepared to explain *why* you diverged from Lua's
    design choices. Respectful framing is key: Nova builds on Lua's
    foundation, it doesn't replace it.

|---------------------------------------------------------------------------------|
4. `Lobsters`
URL: https://lobste.rs
How: Requires invite; submit as a "show" post

`BRIEF`
    A curated, high-quality tech news site. Smaller than HN but the
    audience is more technical and engagement is deeper. If you can
    get an invite (or know someone who has one), this is a strong
    channel. The PL and systems programming tags are active.

|---------------------------------------------------------------------------------|
5. `Twitter/X — #PLDev and #SystemsProgramming`
How: Thread with architecture diagrams, code snippets, benchmarks

`BRIEF`
    A launch thread showing Nova's architecture, key design decisions,
    and a demo GIF of the colored CLI output would perform well. Tag
    relevant people (with genuine context, not spam). The developer
    tools community on Twitter is very active and shares interesting
    projects aggressively.

---
---

## WAVE 3 — Organizations & Enterprise

> Approach these after Wave 1 and Wave 2 have generated initial traction
> (stars, community feedback, maybe a blog mention). Having social proof
> makes these conversations much easier.

|---------------------------------------------------------------------------------|
1. `Redis`
Contact: oss@redis.com (public OSS team inbox)
Also: Antirez individually (see Wave 1, #10)
Website: https://redis.io

`BRIEF`
    Redis embeds Lua for server-side scripting. Nova's compatible syntax,
    improved ergonomics, and built-in data processing (JSON natively)
    could position it as a "next-gen embedded scripting" option. This is
    a long-play conversation — start with Antirez personally, then
    approach the Redis OSS team with traction data.

|---------------------------------------------------------------------------------|
2. `Linux Foundation / LFX`
Contact: info@linuxfoundation.org
Mentorship: https://lfx.linuxfoundation.org/tools/mentorship/

`BRIEF`
    The Linux Foundation hosts mentorship programs for open source
    projects. Getting Nova accepted into LFX Mentorship would provide
    contributors, visibility, and institutional credibility. This is a
    medium-term goal — they want projects with an active community and
    clear contribution guidelines. Prepare CONTRIBUTING.md and good
    first issues before applying.

|---------------------------------------------------------------------------------|
3. `GitLab`
Contact: community@gitlab.com
DevRel: https://about.gitlab.com/community/

`BRIEF`
    GitLab's CI/CD pipeline system could benefit from a lightweight
    scripting runtime. Nova's task runner (taskfile.nini) and tool
    system (built-in grep, find, cat with zero subprocess overhead)
    align with CI/CD automation. The angle is "embedded scripting for
    DevOps pipelines." Mirror the Nova repo on GitLab as a show of
    good faith before reaching out.

|---------------------------------------------------------------------------------|
4. `Microsoft — Developer Relations`
Contact: Scott Hanselman (scott@hanselman.com) — see detailed notes below

`BRIEF`
    The eventual cloud play. Azure Functions, Azure DevOps, and VS Code
    extensions are all potential integration points. But Microsoft DevRel
    responds best when you already have community traction. Scott
    Hanselman is the entry point — he has a podcast (Hanselminutes), a
    massive blog audience, and actively features interesting tools.

`MICROSOFT CONTACTS (DETAILED)`

    a. Scott Hanselman
       Role: Partner Program Manager, Developer Division (DevRel)
       Email: scott@hanselman.com
       Podcast: https://www.hanselminutes.com
       Priority: HIGH — best individual target at Microsoft
       Note: He actively seeks out interesting dev tools. Lead with
       technical differentiators. Keep the email concise. He responds.

    b. Chloe Condon
       Role: Senior Cloud Advocate, Azure DevRel
       Email: chloe@microsoft.com
       Priority: MEDIUM — angle is workflow automation and cloud tooling
       Note: Better fit once Nova has a cloud-native use case to demo.

    c. Jeff Sandquist
       Role: Corporate VP, Developer Relations
       Email: jsandquist@microsoft.com
       Priority: LOW (for now) — VP level, reach after traction
       Note: Approach after Hanselman or another MS contact engages.

    d. Azure Open Source Team
       Email: opensource@microsoft.com
       Priority: LOW — generic inbox, low response rate
       Note: Better reached through a specific person internally.

|---------------------------------------------------------------------------------|
5. `Universities — CS / Compilers / PL Courses`

`BRIEF`
    Nova is an ideal teaching reference: a complete, readable VM in C99
    with clear separation between lexer, parser, compiler, optimizer,
    and VM. University professors teaching compilers or PL courses would
    find it valuable as supplementary material or a project reference.

`TARGETS`

    a. Stanford CS 143 (Compilers)
       Department: https://cs.stanford.edu
       Note: One of the most prestigious compilers courses. Nova's
       pipeline maps directly to the course curriculum.

    b. Cornell CS 4120 (Introduction to Compilers)
       Department: https://www.cs.cornell.edu
       Note: Strong PL group with interest in practical implementations.

    c. Brown CS 1260 (Compilers and Program Analysis)
       Department: https://cs.brown.edu
       Note: Shriram Krishnamurthi's courses emphasize practical PL
       work — he'd be particularly receptive. Reachable via
       sk@cs.brown.edu (public on faculty page).

    d. MIT 6.035 (Computer Language Engineering)
       Department: https://www.eecs.mit.edu
       Note: Focuses on compiler optimization — Nova's optimization
       passes would be relevant course material.

    e. Carnegie Mellon 15-411 (Compiler Design)
       Department: https://www.cs.cmu.edu
       Note: Highly regarded compilers course. Nova's register
       allocation and codegen would interest the instructors.

|---------------------------------------------------------------------------------|
6. `Roc Language / Richard Feldman`
Contact: https://www.roc-lang.org
GitHub: https://github.com/roc-lang/roc
Richard Feldman: https://github.com/rtfeldman

`BRIEF`
    Roc is a functional language focused on "fast, friendly, functional."
    Richard Feldman is building it in public and has a large following
    from his Elm days. The PL community overlap is strong. Not a direct
    collaboration target, but Richard actively engages with other
    language creators and would share Nova with his audience if the
    engineering impressed him.

|---------------------------------------------------------------------------------|
7. `Zig Software Foundation`
Contact: https://ziglang.org/zsf/
Also: Andrew Kelley individually (see Wave 1, #7)

`BRIEF`
    The Zig community values correct, principled systems code. Nova's
    strict C99 with -Werror -pedantic and its zero-dependency approach
    resonates. The Zig Discord and forums are active and would discuss
    Nova's design choices. Approach via community channels first, not
    the foundation directly.

---
---

## Core Messaging

> The pitch that works across all audiences:

    Nova is a complete bytecode virtual machine in 15,000 lines of strict C99.

    Register-based execution with NaN-boxed values, computed-goto dispatch,
    tri-color mark-sweep GC, and a built-in CLI tool ecosystem — one binary,
    zero dependencies.

    Lua-like syntax with modern ergonomics: 0-indexed tables, backtick string
    interpolation, native JSON/CSV/NINI data processing, and an integrated
    task runner.

    Website: nova.dev
    Source:  github.com/zorya-corp/nova

> Adapt the emphasis per audience:
> - PL community      → architecture and design tradeoffs
> - Systems engineers  → C99 discipline, single binary, performance
> - Enterprise / cloud → workflow automation, embedded scripting, data processing
> - Academics          → readable implementation, pedagogical value, clear pipeline stages

---
---

## Tracker

| #  | Name / Channel            | Wave | Status      | Date Sent | Response | Notes |
|----|---------------------------|------|-------------|-----------|----------|-------|
| 1  | Bob Nystrom               | 1    | NOT SENT    |           |          |       |
| 2  | Thorsten Ball             | 1    | NOT SENT    |           |          |       |
| 3  | Justine Tunney            | 1    | NOT SENT    |           |          |       |
| 4  | Roberto Ierusalimschy     | 1    | NOT SENT    |           |          |       |
| 5  | Mike Pall                 | 1    | NOT SENT    |           |          |       |
| 6  | Andy Wingo                | 1    | NOT SENT    |           |          |       |
| 7  | Andrew Kelley             | 1    | NOT SENT    |           |          |       |
| 8  | Chris Lattner             | 1    | NOT SENT    |           |          |       |
| 9  | Brendan Eich              | 1    | NOT SENT    |           |          |       |
| 10 | Antirez                   | 1    | NOT SENT    |           |          |       |
| 11 | Hacker News (Show HN)     | 2    | NOT POSTED  |           |          |       |
| 12 | r/ProgrammingLanguages    | 2    | NOT POSTED  |           |          |       |
| 13 | Lua Mailing List          | 2    | NOT POSTED  |           |          |       |
| 14 | Lobsters                  | 2    | NOT POSTED  |           |          |       |
| 15 | Twitter/X Launch Thread   | 2    | NOT POSTED  |           |          |       |
| 16 | Redis                     | 3    | NOT SENT    |           |          |       |
| 17 | Linux Foundation          | 3    | NOT SENT    |           |          |       |
| 18 | GitLab                    | 3    | NOT SENT    |           |          |       |
| 19 | Scott Hanselman (MS)      | 3    | NOT SENT    |           |          |       |
| 20 | Universities              | 3    | NOT SENT    |           |          |       |
| 21 | Roc / Richard Feldman     | 3    | NOT SENT    |           |          |       |
| 22 | Zig Software Foundation   | 3    | NOT SENT    |           |          |       |

---

`ZORYA CORPORATION — Engineering Excellence, Democratized`
