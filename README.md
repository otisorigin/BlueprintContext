# BlueprintContext

**BlueprintContext** is an Unreal Engine editor plugin that gives LLM-powered coding assistants (Claude Code, Cursor, and any other MCP-compatible client) **project-aware context** about your Blueprints, DataTables, and DataAssets.

> *Other UE MCP servers let LLMs **build** Blueprints. BlueprintContext lets LLMs **understand** the ones you already have.*

Most existing Unreal MCP tools are **agents that act on the editor**: create Blueprints, place actors, wire nodes. BlueprintContext is different — it's a **knowledge layer for developers**. It doesn't modify your project. It indexes what's already there (class hierarchies, variables, functions, components, DataTables, DataAssets, and the full asset reference graph) and exposes it as a queryable API so your LLM can reason about your actual codebase instead of hallucinating against a blank slate.

The API is token-budget aware: every tool paginates, returns compact JSON, and follows a strict "funnel" pattern (broad → specific) so LLMs don't burn context on irrelevant data.

**Tagline:** *Project-aware context for LLM-assisted Unreal development — not an agent, a knowledge layer.*

## How it works

```
UE Editor (plugin)          Python proxy           LLM client
  HTTP :8765         <-->   mcp_proxy.py    <-->   stdio MCP protocol
  SQLite index              translates HTTP         (Claude Code, Cursor, ...)
  (blueprints.db)           <-> MCP JSON-RPC
```

1. The plugin starts an HTTP server inside the UE Editor on port `8765`.
2. On editor startup it indexes every Blueprint, DataTable, and DataAsset into a local SQLite database and builds a relationship graph (inheritance, interface implementation, DT/DA usage, hard/soft references).
3. `mcp_proxy.py` (pure stdlib Python) bridges between the MCP stdio protocol and the plugin's HTTP API.
4. Any MCP-compatible client launches the proxy and gets access to the tool set.

Everything lives in your project — no cloud calls, no external services, no extra dependencies beyond stdlib Python and UE's bundled `SQLiteCore`.

## Features

- **Architecture overview** — summary counts of class families, interfaces, and folder groups with drill-down pagination
- **Semantic Blueprint search** — find classes by natural-language queries
- **Inheritance & relationship graph** — walk ancestors, descendants, implementors, and package references recursively
- **Blueprint inspection** — variables, parent-class properties (with override detection), functions, components, full C++-style header
- **DataTable support** — list tables, inspect schemas, pull specific rows lazily (never the whole table unless asked)
- **DataAsset support** — read field values without loading the editor UI
- **Token-aware design** — every list endpoint paginates, every response uses compact keys, and tools refuse obviously wasteful calls
- **Auto-reindex on compile** — the plugin keeps the index fresh as you work
- **Auth token** — regenerated every editor session, stored in `Saved/BlueprintMCP/token.txt`

## Requirements

- Unreal Engine **5.7+**
- **Python 3.8+** on PATH (for the MCP proxy — stdlib only, no pip install needed)
- An MCP-compatible client (Claude Code, Cursor, etc.)

## Installation

### 1. Drop the plugin into your project

Copy `Plugins/BlueprintMCP/` into your project's `Plugins/` folder (or add this repo as a submodule). Regenerate project files and build — the plugin compiles as part of the project.

### 2. Copy the proxy and MCP config to your project root

- `mcp_proxy.py` — the stdio ↔ HTTP bridge
- `.mcp.json` — tells MCP clients how to launch the proxy

Minimal `.mcp.json`:

```json
{
  "mcpServers": {
    "blueprint-context": {
      "command": "python",
      "args": ["mcp_proxy.py"]
    }
  }
}
```

Claude Code auto-discovers `.mcp.json` when you `cd` into the project. On systems where `python` resolves to Python 2, change `"python"` to `"python3"`.

### 3. Open the UE Editor

The plugin starts automatically. Check the Output Log for:

```
[BlueprintMCP] Ready. N blueprints, M datatables, K dataassets indexed. E edges built. Server started.
```

### 4. Launch your MCP client in the project directory

```bash
cd /path/to/your/project
claude
```

Ask the assistant something like *"use bp_architecture to show me the project structure"* — if you see class families, interfaces, and folders, you're connected.

## Usage

The plugin exposes a token-budgeted tool set. The rule of thumb is to **funnel from broad to specific**:

```
bp_architecture()                 <- summary counts
bp_architecture(group="...")      <- drill into a group
        │
bp_search / dt_list               <- find specific assets
        │
bp_hierarchy / asset_related      <- understand relationships
        │
bp_funcs / bp_vars / dt_schema    <- inspect interface
        │
bp_header / dt_rows / da_values   <- full details (only when needed)
```

### Main tools

| Tool | Purpose |
|---|---|
| `bp_architecture` | Project overview — families, interfaces, folders, with pagination |
| `bp_search(query)` | Semantic Blueprint search |
| `bp_list(filter)` | Filtered Blueprint listing (filter is required) |
| `dt_list` / `dt_schema` / `dt_rows` | DataTable discovery and lazy row access |
| `da_values` | DataAsset field values |
| `bp_hierarchy` | Ancestors + children tree |
| `bp_vars` / `bp_parent_vars` | Own variables / inherited properties (with `only_overridden` filter) |
| `bp_funcs` | Function signatures |
| `bp_components` | Components with class and source |
| `bp_header` | Full C++-style header (use sparingly) |
| `bp_refs` | Children or interface implementors |
| `asset_related` | Universal graph query across any edge type — `INHERITS`, `IMPLEMENTS`, `USES_DT`, `USES_DA`, `HAS_VAR_OF_TYPE`, `REFERENCES` |

All list endpoints return `total`, `returned`, and `has_more`; default `limit` is `20`.

### Example prompts

- *"What are the main Blueprint families in this project?"*
- *"Find all enemies that inherit from BP_EnemyBase recursively."*
- *"Which Blueprints use DT_EnemyStats?"*
- *"Show me the overridden properties on BP_BossGoblin."*
- *"What meshes and animations does BP_PlayerCharacter reference?"*

See `Plugins/BlueprintMCP/CLAUDE.md` for the full tool reference, token budgets, forbidden patterns, and usage scenarios. Drop a similar `CLAUDE.md` (or equivalent) in your own project root so your LLM follows the funnel rule automatically.

## Configuration

`Plugins/BlueprintMCP/Config/DefaultEditor.ini`:

```ini
[BlueprintMCP]
Port=8765
IndexDataTables=true
IndexDataAssets=true
IndexDataAssetLoadFull=false
MaxDtRowsPerRequest=20
MaxRelatedPerRequest=50
DbWALMode=true
; ExtraIncludePaths=/SharedAssets/,/CompanyLib/
ExcludeFolders=/Game/Art/,/Game/Audio/,/Game/VFX/,/Game/Cinematics/,/Game/Movies/
; ExtraSkipClasses=/Script/MyPlugin.MyHeavyAsset
```

## Console commands

| Command | Purpose |
|---|---|
| `BlueprintMCP.DbInfo` | Print asset/edge counts |
| `BlueprintMCP.IndexStats` | Classification breakdown (`verbose` for top-10) |
| `BlueprintMCP.PrintIndex` | Print all indexed blueprints |
| `BlueprintMCP.RebuildIndex` | Force full reindex |
| `BlueprintMCP.RebuildEdges` | Rebuild the relationship graph |
| `BlueprintMCP.PrintEdges <path>` | Show all edges for an asset |
| `BlueprintMCP.Search <query>` | Test search from the console |

## Troubleshooting

| Problem | Fix |
|---|---|
| *Connection refused* | UE Editor must be running; make sure port `8765` isn't blocked or taken |
| *Invalid token* | Token regenerates each editor session; the proxy reads `Saved/BlueprintMCP/token.txt` automatically — just restart the client |
| *Tools not showing in Claude Code* | Restart the client after the editor is fully loaded; confirm `.mcp.json` and `mcp_proxy.py` sit in the project root |
| *`python` not found* | Use `python3` in `.mcp.json`, or add Python to PATH |
| *Stale data after Blueprint changes* | The plugin auto-reindexes on compile; run `BlueprintMCP.RebuildIndex` to force |

## Project layout

```
ProjectRoot/
├── .mcp.json                     MCP server config (auto-discovered by Claude Code)
├── mcp_proxy.py                  stdio <-> HTTP proxy (stdlib only)
├── Saved/BlueprintMCP/
│   ├── token.txt                 auth token (per session)
│   ├── blueprints.db             SQLite index
│   └── headers/                  cached .h files
└── Plugins/BlueprintMCP/
    ├── BlueprintMCP.uplugin
    ├── Config/DefaultEditor.ini
    └── Source/BlueprintMCP/      C++ source
```

## License

TBD — add your preferred license before publishing.

## Contributing

Issues and PRs welcome. If you add a new tool, please keep the token-budget philosophy: paginate by default, prefer compact keys, and document the expected cost per call.
