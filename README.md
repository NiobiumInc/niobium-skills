# Niobium Skills

AI Agent Skills by Niobium — portable [`SKILL.md`](https://agentskills.io/specification)
packages that guide AI coding agents through building privacy-preserving and fully
homomorphic encryption (FHE) applications. Each skill works with any agent that supports
the open Agent Skills standard: Claude Code, OpenAI Codex CLI, Cursor, Gemini CLI, GitHub
Copilot, Windsurf, and more.

## Skills

### [`fhe-application-design`](skills/fhe-application-design/) — design FHE applications

A ten-stage methodology (Stages 0–9) for designing and building Fully Homomorphic
Encryption (FHE) applications with OpenFHE (optional Niobium DSL path) — from build
environment and privacy model through feasibility, scheme selection, circuit design,
parameter selection, the faithful twin, encrypted implementation, and protocol
specification. Use it whenever you need to compute on encrypted data, assess FHE
feasibility, or structure a client–server protocol for encrypted computation.

```bash
npx skills add NiobiumInc/niobium-skills --skill fhe-application-design
```

**Prerequisites (beyond the skill itself):** building and running the encrypted
application (Stage 8) happens in the prebuilt **FHE-dev container** — you never build
OpenFHE from source. You need [Docker](https://www.docker.com/products/docker-desktop/)
and a one-time pull of the image (several GB):

```bash
docker pull ghcr.io/niobiuminc/fhe-dev:v0.6.0
docker run --rm ghcr.io/niobiuminc/fhe-dev:v0.6.0 fhe-smoke-test   # ends with SMOKE OK
```

The skill walks you through this as its Stage 0, so you can also just start a
conversation and let the agent set it up with you. The design stages (1–7) need nothing
installed at all.

See the [skill's README](skills/fhe-application-design/) for full documentation.

| Skill | What it does | Install |
| --- | --- | --- |
| [`fhe-application-design`](skills/fhe-application-design/) | Design & build FHE applications (OpenFHE / optional Niobium DSL) | `npx skills add NiobiumInc/niobium-skills --skill fhe-application-design` |

## Installation

Each skill installs independently. Pick whichever method fits your setup — all of them
install the full skill folder, including its `references/`.

### Universal CLI (recommended)

```bash
npx skills add NiobiumInc/niobium-skills --skill <skill-name>
```

[skills.sh](https://skills.sh) auto-detects your agent and writes the skill to the right
directory.

### GitHub CLI

```bash
gh skill install NiobiumInc/niobium-skills <skill-name>
```

### Claude Code plugin marketplace

This repo is also a Claude Code plugin marketplace, so each skill can be installed as a
plugin:

```
/plugin marketplace add NiobiumInc/niobium-skills
/plugin install fhe-application-design@niobium-skills
```

### Manual

Clone the catalog and copy the skill folder (preserving `references/`) into your agent's
skills directory:

```bash
git clone https://github.com/NiobiumInc/niobium-skills
cp -r niobium-skills/skills/<skill-name> ~/.claude/skills/<skill-name>
```

| Agent | Per-project | Per-user |
| --- | --- | --- |
| Claude family | `.claude/skills/<skill-name>/` | `~/.claude/skills/<skill-name>/` |
| Codex / agentskills.io agents (Cursor, Copilot, Windsurf, Gemini, Cline) | `.agents/skills/<skill-name>/` | `~/.agents/skills/<skill-name>/` |

The installed directory name must match the skill's `name` frontmatter. For any agent not
listed, use the `npx skills add` installer above or see
[agentskills.io](https://agentskills.io).

## Repository structure

```
niobium-skills/
├── README.md                          # This catalog landing page
├── LICENSE                            # Apache-2.0 (applies to all skills)
├── CONTRIBUTING.md
├── .claude-plugin/
│   └── marketplace.json               # Claude Code plugin marketplace manifest
└── skills/
    └── fhe-application-design/        # A self-contained skill (name matches its dir)
        ├── SKILL.md
        ├── README.md
        ├── references/
        └── evals/
```

Each skill is self-contained under `skills/<name>/`, with its own `SKILL.md`, `README.md`,
and bundled `references/`. Adding a skill is one folder here plus one entry in
`.claude-plugin/marketplace.json`.

## License

[Apache-2.0](LICENSE). Maintained by [Niobium](https://niobium.co).
