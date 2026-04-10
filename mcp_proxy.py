#!/usr/bin/env python3
"""MCP stdio proxy for Blueprint MCP HTTP server at localhost:8765."""
import sys
import json
import urllib.request
import urllib.error
import os

BASE_URL = "http://localhost:8765"
TOKEN_PATH = os.path.join(os.path.dirname(os.path.abspath(__file__)), "Saved", "BlueprintMCP", "token.txt")

TOOLS = [
    {
        "name": "bp_search",
        "description": "Semantic search across all project blueprints by query string",
        "inputSchema": {
            "type": "object",
            "properties": {
                "query": {"type": "string", "description": "Search query"},
                "top_k": {"type": "integer", "description": "Max results (1-20, default 10)"}
            },
            "required": ["query"]
        }
    },
    {
        "name": "bp_architecture",
        "description": "Project architecture overview. Without 'group': returns summary counts only (~80 tokens). With 'group': returns paginated items for that section.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "group": {"type": "string", "description": "Section to retrieve: 'families', 'interfaces', or 'folders'. Omit for summary counts only."},
                "folder": {"type": "string", "description": "Filter to folder substring"},
                "min_children": {"type": "integer", "description": "Min children to show a family (default 2, families group only)"},
                "limit": {"type": "integer", "description": "Max items in the group (1-100, default 20)"},
                "offset": {"type": "integer", "description": "Skip first N items (default 0)"}
            }
        }
    },
    {
        "name": "bp_list",
        "description": "List blueprints with filters. Returns total/returned/has_more for pagination. At least one filter required or limit<=100.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "name_glob": {"type": "string", "description": "Glob pattern for blueprint name"},
                "parent": {"type": "string", "description": "Filter by parent class name"},
                "interface": {"type": "string", "description": "Filter by implemented interface"},
                "folder": {"type": "string", "description": "Filter by folder path substring"},
                "limit": {"type": "integer", "description": "Max items (1-100, default 20)"},
                "offset": {"type": "integer", "description": "Skip first N results (default 0)"},
                "fields": {"type": "array", "items": {"type": "string"}, "description": "Subset of fields: name,path,parent,folder,interfaces,var_names,func_names"}
            }
        }
    },
    {
        "name": "bp_hierarchy",
        "description": "Inheritance tree for a specific blueprint class",
        "inputSchema": {
            "type": "object",
            "properties": {"path": {"type": "string", "description": "Blueprint asset path"}},
            "required": ["path"]
        }
    },
    {
        "name": "bp_header",
        "description": "Full C++-style header declaration for a blueprint",
        "inputSchema": {
            "type": "object",
            "properties": {"path": {"type": "string", "description": "Blueprint asset path"}},
            "required": ["path"]
        }
    },
    {
        "name": "bp_vars",
        "description": "Variables declared directly in this blueprint class (NewVariables only). Empty if the BP adds no new variables — use bp_parent_vars to see inherited properties. Returns total/returned/has_more.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "path": {"type": "string", "description": "Blueprint asset path"},
                "limit": {"type": "integer", "description": "Max vars (1-100, default 20)"},
                "offset": {"type": "integer", "description": "Skip first N vars (default 0)"}
            },
            "required": ["path"]
        }
    },
    {
        "name": "bp_parent_vars",
        "description": "Editor-visible properties from all parent project classes (Blueprint and C++) up to the first engine class. Shows value set in this BP and flags overridden=true when the default differs from the parent's default. Returns total/returned/has_more for pagination.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "path": {"type": "string", "description": "Blueprint asset path"},
                "only_overridden": {"type": "boolean", "description": "Only show vars where this BP changed the default (default false)"},
                "category": {"type": "string", "description": "Filter by property category substring"},
                "limit": {"type": "integer", "description": "Max vars (1-100, default 20)"},
                "offset": {"type": "integer", "description": "Skip first N vars (default 0)"}
            },
            "required": ["path"]
        }
    },
    {
        "name": "bp_funcs",
        "description": "Functions for a blueprint class. Returns total/returned/has_more.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "path": {"type": "string", "description": "Blueprint asset path"},
                "limit": {"type": "integer", "description": "Max functions (1-100, default 20)"},
                "offset": {"type": "integer", "description": "Skip first N (default 0)"}
            },
            "required": ["path"]
        }
    },
    {
        "name": "bp_refs",
        "description": "Reverse references: children, interface implementors, or any edge type (via SQL graph)",
        "inputSchema": {
            "type": "object",
            "properties": {
                "path": {"type": "string", "description": "Blueprint asset path"},
                "ref_type": {"type": "string", "description": "child | interface_impl | INHERITS | IMPLEMENTS | USES_DT | USES_DA | HAS_VAR_OF_TYPE | REFERENCES (default: child)"},
                "recursive": {"type": "boolean", "description": "Traverse recursively (default: false)"},
                "limit": {"type": "integer", "description": "Max results (1-50, default 50)"}
            },
            "required": ["path"]
        }
    },
    {
        "name": "asset_related",
        "description": "Universal graph traversal: find related assets by relationship type and direction, with optional recursion",
        "inputSchema": {
            "type": "object",
            "properties": {
                "path": {"type": "string", "description": "Asset path (Blueprint, DataTable, or DataAsset)"},
                "rel": {"type": "string", "description": "INHERITS | IMPLEMENTS | USES_DT | USES_DA | HAS_VAR_OF_TYPE | REFERENCES | all"},
                "direction": {"type": "string", "description": "in (who references us) | out (what we reference) | both"},
                "recursive": {"type": "boolean", "description": "Traverse recursively (default: false)"},
                "limit": {"type": "integer", "description": "Max results per page (1-50, default 50)"},
                "offset": {"type": "integer", "description": "Skip first N results for pagination (default 0)"}
            },
            "required": ["path", "rel", "direction"]
        }
    },
    {
        "name": "dt_list",
        "description": "List DataTables with optional filters. Returns total/returned/has_more.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "row_struct": {"type": "string", "description": "Filter by row struct name (e.g. FEnemyStatsRow)"},
                "folder": {"type": "string", "description": "Filter by folder path substring"},
                "limit": {"type": "integer", "description": "Max tables (1-100, default 20)"},
                "offset": {"type": "integer", "description": "Skip first N (default 0)"}
            }
        }
    },
    {
        "name": "dt_schema",
        "description": "Column names and types for a DataTable (no LoadObject needed)",
        "inputSchema": {
            "type": "object",
            "properties": {"path": {"type": "string", "description": "DataTable asset path"}},
            "required": ["path"]
        }
    },
    {
        "name": "dt_rows",
        "description": "Fetch specific rows from a DataTable (lazy loaded, cached, max 20 default)",
        "inputSchema": {
            "type": "object",
            "properties": {
                "path": {"type": "string", "description": "DataTable asset path"},
                "rows": {"type": "array", "items": {"type": "string"}, "description": "Specific row names to fetch (omit for first 20)"},
                "columns": {"type": "array", "items": {"type": "string"}, "description": "Filter to specific columns (omit for all)"}
            },
            "required": ["path"]
        }
    },
    {
        "name": "da_values",
        "description": "All UPROPERTY field values for a DataAsset",
        "inputSchema": {
            "type": "object",
            "properties": {"path": {"type": "string", "description": "DataAsset asset path"}},
            "required": ["path"]
        }
    },
    {
        "name": "asset_properties",
        "description": "All UPROPERTY field values for ANY asset type — use this for InputMappingContext, InputAction, CurveTable, StringTable, or any Generic asset that da_values doesn't cover",
        "inputSchema": {
            "type": "object",
            "properties": {"path": {"type": "string", "description": "Asset object path (e.g. /Game/Input/IMC_Default.IMC_Default)"}},
            "required": ["path"]
        }
    },
    {
        "name": "bp_components",
        "description": "List all components in a Blueprint (own + inherited), with class name and source. Returns total/returned/has_more.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "path": {"type": "string", "description": "Blueprint asset path"},
                "limit": {"type": "integer", "description": "Max components (1-100, default 20)"},
                "offset": {"type": "integer", "description": "Skip first N (default 0)"}
            },
            "required": ["path"]
        }
    },
]

TOOL_ENDPOINTS = {t["name"]: f"/mcp/{t['name']}" for t in TOOLS}


def read_token():
    try:
        with open(TOKEN_PATH, 'r') as f:
            return f.read().strip()
    except Exception:
        return ""


def http_post(endpoint, body):
    token = read_token()
    url = BASE_URL + endpoint
    data = json.dumps(body).encode('utf-8')
    req = urllib.request.Request(url, data=data, method='POST')
    req.add_header('Content-Type', 'application/json')
    req.add_header('Authorization', f'Bearer {token}')
    try:
        with urllib.request.urlopen(req, timeout=120) as resp:
            return json.loads(resp.read().decode('utf-8')), None
    except urllib.error.HTTPError as e:
        return None, {"code": e.code, "message": e.read().decode('utf-8')}
    except Exception as e:
        return None, {"code": -1, "message": str(e)}


def send(msg):
    sys.stdout.write(json.dumps(msg) + '\n')
    sys.stdout.flush()


def handle(msg):
    method = msg.get('method', '')
    msg_id = msg.get('id')

    if method == 'initialize':
        send({"jsonrpc": "2.0", "id": msg_id, "result": {
            "protocolVersion": "2024-11-05",
            "capabilities": {"tools": {}},
            "serverInfo": {"name": "blueprint-mcp", "version": "1.0.0"}
        }})

    elif method == 'notifications/initialized':
        pass  # notification, no response

    elif method == 'tools/list':
        send({"jsonrpc": "2.0", "id": msg_id, "result": {"tools": TOOLS}})

    elif method == 'tools/call':
        params = msg.get('params', {})
        tool_name = params.get('name', '')
        arguments = params.get('arguments', {})

        endpoint = TOOL_ENDPOINTS.get(tool_name)
        if not endpoint:
            send({"jsonrpc": "2.0", "id": msg_id,
                  "error": {"code": -32601, "message": f"Unknown tool: {tool_name}"}})
            return

        result, err = http_post(endpoint, arguments)
        if err:
            send({"jsonrpc": "2.0", "id": msg_id, "error": err})
        else:
            content = result.get('result', result)
            # Compact JSON for high-volume endpoints to save LLM tokens
            compact_tools = {'asset_related', 'bp_list', 'bp_search'}
            indent = None if tool_name in compact_tools else 2
            send({"jsonrpc": "2.0", "id": msg_id, "result": {
                "content": [{"type": "text", "text": json.dumps(content, indent=indent)}]
            }})

    else:
        if msg_id is not None:
            send({"jsonrpc": "2.0", "id": msg_id,
                  "error": {"code": -32601, "message": f"Method not found: {method}"}})


def main():
    for line in sys.stdin:
        line = line.strip()
        if not line:
            continue
        try:
            msg = json.loads(line)
            handle(msg)
        except Exception:
            pass


if __name__ == '__main__':
    main()
