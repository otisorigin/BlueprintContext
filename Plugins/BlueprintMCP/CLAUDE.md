# Blueprint MCP Plugin — Setup & Usage Guide

## What this is

Blueprint MCP is an Unreal Engine editor plugin that exposes Blueprint, DataTable, and DataAsset metadata to LLM tools (Claude Code, Cursor, etc.) via an HTTP API + MCP protocol. It lets AI assistants inspect your project's Blueprint architecture without needing screenshots or manual descriptions.

## How it works

```
UE Editor (plugin)          Python proxy           LLM (Claude Code)
  HTTP :8765         <-->   mcp_proxy.py    <-->   stdio MCP protocol
  SQLite DB                 translates HTTP         reads tool results
  (blueprints.db)           to/from MCP
```

1. The plugin runs an HTTP server inside UE Editor on port 8765
2. On startup it indexes all Blueprints, DataTables, DataAssets into a SQLite database
3. `mcp_proxy.py` bridges between the HTTP API and the MCP stdio protocol
4. Claude Code (or any MCP client) communicates with the proxy via stdin/stdout

## Setup for a new machine

### Prerequisites

- Unreal Engine 5.7+ with this project
- Python 3.8+ (for the proxy)
- Claude Code CLI (or any MCP-compatible tool)

### Step 1: Build the project

Open the `.uproject` in UE Editor or build from command line. The plugin compiles as part of the project. No extra dependencies needed — SQLite is bundled in UE's `SQLiteCore` module.

### Step 2: Verify `.mcp.json` exists in project root

```json
{
  "mcpServers": {
    "blueprint-mcp": {
      "command": "python",
      "args": ["mcp_proxy.py"]
    }
  }
}
```

Claude Code auto-discovers this file when you `cd` into the project directory. The path is relative — works on any machine.

### Step 3: Open UE Editor

The plugin starts automatically when the editor opens. Check the Output Log for:

```
[BlueprintMCP] Ready. N blueprints, M datatables, K dataassets indexed. E edges built. Server started.
```

### Step 4: Start Claude Code in the project directory

```bash
cd /path/to/project
claude
```

Claude Code reads `.mcp.json`, launches `mcp_proxy.py`, and the Blueprint MCP tools become available.

### Step 5: Verify connection

Ask Claude: "use bp_architecture to show me the project structure"

If it works, you'll see class families, interfaces, and folder structure.

## Troubleshooting

| Problem | Fix |
|---------|-----|
| "Connection refused" | UE Editor must be running. Check port 8765 is not blocked. |
| "Invalid token" | Token regenerates on each editor start. Proxy reads it from `Saved/BlueprintMCP/token.txt` automatically. |
| Tools not showing | Restart Claude Code after editor is running. Check `mcp_proxy.py` exists in project root. |
| "python not found" | Ensure `python` is on PATH. On some systems use `python3`. Update `.mcp.json` accordingly. |
| Stale data after BP changes | Plugin auto-reindexes on compile. Run `BlueprintMCP.RebuildIndex` in UE console to force. |

## Files

```
ProjectRoot/
├── .mcp.json                    — MCP server config (auto-discovered by Claude Code)
├── mcp_proxy.py                 — stdio-to-HTTP proxy (no dependencies beyond stdlib)
├── Saved/BlueprintMCP/
│   ├── token.txt                — auth token (auto-generated each editor session)
│   ├── blueprints.db            — SQLite index (auto-created)
│   └── headers/                 — cached .h files
└── Plugins/BlueprintMCP/
    ├── BlueprintMCP.uplugin
    └── Source/BlueprintMCP/      — C++ source
```

## Available tools

### Discovery & Search

| Tool | Purpose | Token cost |
|------|---------|-----------|
| `bp_architecture` | Project architecture. No params = summary counts (~80 tok). `group`=families/interfaces/folders for paginated details. Params: `group`, `folder`, `min_children`, `limit`(20), `offset` | ~80 summary, ~50/family |
| `bp_search(query)` | Find blueprints by semantic query | ~40/result |
| `bp_list(filter)` | List blueprints with filters. Params: `limit`(20), `offset`, `fields[]` | ~25/item |
| `dt_list(row_struct?, folder?)` | List DataTables. Params: `limit`(20), `offset` | ~30/table |

### Inspection

All inspection tools return `total`, `returned`, `has_more` for pagination. Default `limit`=20.

| Tool | Purpose | Token cost |
|------|---------|-----------|
| `bp_hierarchy(path)` | Inheritance tree (ancestors + children) | ~300 |
| `bp_vars(path)` | Variables declared directly in this BP. Params: `limit`(20), `offset` | ~30/var |
| `bp_parent_vars(path)` | Parent class properties with override flags. Params: `limit`(20), `offset`, `only_overridden`(bool), `category`(string) | ~40/var |
| `bp_funcs(path)` | Functions with signatures. Params: `limit`(20), `offset` | ~40/func |
| `bp_header(path)` | Full C++-style header (vars + funcs). Use sparingly. | ~500 |
| `dt_schema(path)` | DataTable column names and types (no load) | ~100 |
| `dt_rows(path, rows?, columns?)` | DataTable row values (lazy loaded, max 20) | ~50/row |
| `bp_components(path)` | Component list with class names and source. Params: `limit`(20), `offset` | ~30/component |
| `da_values(path)` | DataAsset field values | ~200 |

### Relationships

| Tool | Purpose | Token cost |
|------|---------|-----------|
| `bp_refs(path)` | Children or interface implementors | ~40/ref |
| `asset_related(path, rel, direction)` | Universal graph query (any asset type, any edge type, recursive) | ~30/result |

**Relationship types for `asset_related`:** INHERITS, IMPLEMENTS, USES_DT, USES_DA, HAS_VAR_OF_TYPE, REFERENCES, all

**Directions:** `in` (who references this asset), `out` (what this asset references), `both`

### asset_related compact response format

Response uses short keys for token efficiency. Paths have `/Game/` prefix stripped. C++ class paths resolved (e.g. `AnimMontage` not `/Script/Engine.AnimMontage`). Empty/default fields omitted.

| Key  | Meaning        | Omitted when        |
|------|---------------|---------------------|
| `n`  | name           | never                |
| `p`  | path (no /Game/) | never             |
| `t`  | asset type     | never                |
| `r`  | relationship   | never                |
| `d`  | depth          | depth = 1 (default)  |
| `pp` | property_path  | empty                |
| `h`  | dep hardness (Hard/Soft) | empty       |

**Asset types for content refs:** StaticMesh, SkeletalMesh, AnimMontage, AnimSequence, BlendSpace, Texture2D, Material, MaterialInstance, SoundCue, SoundWave, NiagaraSystem, ParticleSystem

## Usage rules (important for LLMs)

### Funnel rule — go from broad to specific

```
bp_architecture()        <- summary counts (families/interfaces/folders)
bp_architecture(group=)  <- drill into a specific group with pagination
       |
bp_search / dt_list      <- find specific assets
       |
bp_hierarchy / asset_related  <- understand relationships
       |
bp_funcs / bp_vars / bp_parent_vars / dt_schema  <- inspect interface
       |
bp_header / dt_rows / da_values  <- full details (only when needed)
```

### Pagination pattern

All list endpoints return `total`, `returned`, and `has_more`. Default limit = 20.
Use `limit` and `offset` to paginate. Check `total` first to decide if you need more pages.

Tip: `bp_parent_vars(path, only_overridden=true)` shows just customized values (~80% fewer tokens).

### Do NOT

- Call `bp_list` without a filter — server rejects it
- Call `bp_header` without knowing the class name — use `bp_search` first
- Call `bp_header` for more than 5 blueprints in one step
- Call `dt_rows` without `rows` parameter if table has >20 rows
- Call `asset_related` without specifying `rel` and `direction`
- Loop through classes to "see what's there" — use `bp_architecture`
- Paginate unnecessarily — check `total` first, often the default page is enough

## Console commands (UE Editor)

| Command | Purpose |
|---------|---------|
| `BlueprintMCP.DbInfo` | Print asset/edge counts |
| `BlueprintMCP.IndexStats` | Classification breakdown (add `verbose` for Generic top-10) |
| `BlueprintMCP.PrintIndex` | Print all indexed blueprints |
| `BlueprintMCP.RebuildIndex` | Force full reindex |
| `BlueprintMCP.RebuildEdges` | Rebuild relationship graph |
| `BlueprintMCP.PrintEdges <path>` | Show all edges for an asset |
| `BlueprintMCP.Search <query>` | Test search from console |

## Configuration (Config/DefaultEditor.ini)

```ini
[BlueprintMCP]
Port=8765
IndexDataTables=true
IndexDataAssets=true
IndexDataAssetLoadFull=false
MaxDtRowsPerRequest=20
MaxRelatedPerRequest=50
; DbPath auto-resolves to ProjectSaved/BlueprintMCP/blueprints.db
DbWALMode=true
; Extra mount points beyond /Game/ (for shared content)
; ExtraIncludePaths=/SharedAssets/,/CompanyLib/
; Folders inside /Game/ to skip
ExcludeFolders=/Game/Art/,/Game/Audio/,/Game/VFX/,/Game/Cinematics/,/Game/Movies/
; Extra classes for Skip
; ExtraSkipClasses=/Script/MyPlugin.MyHeavyAsset
```
