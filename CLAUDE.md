# Blueprint MCP

Blueprint classes, DataTables and DataAssets are available via MCP server on localhost:8765.
Token: Saved/BlueprintMCP/token.txt

## Funnel rule (strictly follow)

```
bp_architecture()        <- summary counts (families/interfaces/folders)
bp_architecture(group=)  <- drill into a specific group with pagination
       |
bp_search / dt_list      <- find a specific asset by meaning
       |
bp_hierarchy             <- Blueprint inheritance tree
asset_related            <- relationships between any assets
       |
bp_funcs | bp_vars       <- Blueprint interface
dt_schema                <- DataTable columns
       |
bp_header                <- full C++ interface
dt_rows(rows=[...])      <- specific DataTable rows (only the ones you need!)
da_values                <- DataAsset fields
```

## Pagination

All list endpoints return `total`, `returned`, `has_more`. Default limit = 20.
Use `limit` and `offset` for pagination.

- `bp_parent_vars(path, only_overridden=true)` — show only changed values (~80% token savings)
- `bp_parent_vars(path, category="Combat")` — filter by property category
- `bp_list(parent="Actor", limit=10, offset=10)` — second page
- `bp_list(..., fields=["name","path"])` — only requested fields

## Forbidden

- `bp_list` without a filter — server will return an error
- `bp_header` without knowing the class name — use `bp_search` first
- `bp_header` for more than 5 blueprints in one step
- `dt_rows` without `rows` parameter if row count > 20 — will blow up context
- `asset_related` without specifying `rel` and `direction` — too broad
- Looping through classes to "see what's there" — use `bp_architecture` for that
- Paginating unnecessarily — check `total` first, often the default page is enough

## Relationship types (for asset_related)

| rel             | Meaning                                  | direction |
|----------------|------------------------------------------|-----------|
| INHERITS        | Blueprint inheritance                    | in/out    |
| IMPLEMENTS      | interface implementation                 | in/out    |
| USES_DT         | Blueprint uses a DataTable               | in/out    |
| USES_DA         | Blueprint uses a DataAsset               | in/out    |
| HAS_VAR_OF_TYPE | variable type points to another class    | in/out    |
| REFERENCES      | any package dependency (meshes, anims, sounds, etc.) | in/out |

## asset_related response format (compact)

Response uses short keys for token efficiency. Paths have `/Game/` stripped.

```json
{"results":[
  {"n":"AM_Death_01","p":"Characters/Anims/AM_Death_01.AM_Death_01","t":"AnimMontage","r":"REFERENCES","h":"Hard"},
  {"n":"BP_EnemyBase","p":"Enemies/BP_EnemyBase.BP_EnemyBase_C","t":"Blueprint","r":"INHERITS","d":2}
]}
```

| Key  | Meaning        | Omitted when        |
|------|---------------|---------------------|
| `n`  | name           | never                |
| `p`  | path (no /Game/) | never             |
| `t`  | asset type     | never                |
| `r`  | relationship   | never                |
| `d`  | depth          | depth = 1 (default)  |
| `pp` | property_path  | empty                |
| `h`  | dep hardness (Hard/Soft) | empty       |

**Pagination:** `meta` contains `returned`, `has_more` (omitted if false), `offset` (omitted if 0).

```
asset_related(path, rel="REFERENCES", direction="out", limit=20, offset=0)   <- page 1
asset_related(path, rel="REFERENCES", direction="out", limit=20, offset=20)  <- page 2
```

## Scenario A — "Find the right class/table"

```
bp_search("enemy damage health")           <- for Blueprints
dt_list(row_struct="FEnemyStatsRow")       <- for DataTables
  -> bp_funcs / dt_schema for top 3
  -> bp_header / dt_rows(rows=[...]) if details needed
```

## Scenario B — "Understand architecture"

```
bp_architecture()                                  <- get summary counts
bp_architecture(group="families", limit=10)        <- first 10 class families
bp_architecture(group="interfaces")                <- all interfaces
  -> asset_related(path, rel="INHERITS", direction="in", recursive=true)
  -> asset_related(path, rel="USES_DT", direction="out")
```

## Scenario C — "Find all usages"

```
// Who uses this DataTable?
asset_related("/Game/Data/DT_EnemyStats", rel="USES_DT", direction="in")

// What does this Blueprint use?
asset_related("/Game/BP_EnemyBase", rel="all", direction="out")

// All descendants recursively
asset_related("/Game/BP_EnemyBase", rel="INHERITS", direction="in", recursive=true)
```

## Token budgets

| Tool             | Tokens/call     | Limit                          |
|----------------|----------------|-------------------------------|
| `bp_architecture` | ~80 (summary), ~50/family, ~20/iface, ~15/folder | group, folder, min_children, limit, offset |
| `bp_search`       | ~40/result      | top_k <= 20                    |
| `asset_related`   | ~30/result      | limit=50, offset, depth<=10    |
| `bp_hierarchy`    | ~300            | depth<=5                       |
| `dt_list`         | ~30/table       | limit=20, offset               |
| `dt_schema`       | ~100            | —                              |
| `bp_vars`         | ~30/item        | limit=20, offset               |
| `bp_parent_vars`  | ~40/item        | limit=20, offset, only_overridden, category |
| `bp_funcs`        | ~40/item        | limit=20, offset               |
| `bp_components`   | ~30/item        | limit=20, offset               |
| `bp_header`       | ~500            | warn >1500                     |
| `dt_rows`         | ~50/row         | max 20 rows without rows=[]    |
| `da_values`       | ~200            | —                              |
