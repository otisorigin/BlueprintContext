#include "MCPDatabase.h"
#include "SQLiteDatabase.h"
#include "SQLitePreparedStatement.h"
#include "Misc/Paths.h"
#include "HAL/PlatformFileManager.h"

DEFINE_LOG_CATEGORY_STATIC(LogMCPDatabase, Log, All);

// ============================================================
// Schema SQL
// ============================================================

static const TCHAR* GCreateSchemaSQL[] = {
	// --- assets ---
	TEXT(R"(
		CREATE TABLE IF NOT EXISTS assets (
			id           INTEGER PRIMARY KEY AUTOINCREMENT,
			name         TEXT    NOT NULL,
			path         TEXT    NOT NULL UNIQUE,
			asset_type   TEXT    NOT NULL,
			parent       TEXT,
			folder       TEXT,
			modified_at  INTEGER NOT NULL,
			header_hash  TEXT,
			row_struct   TEXT,
			row_count    INTEGER,
			da_class     TEXT
		)
	)"),
	// --- asset_meta ---
	TEXT(R"(
		CREATE TABLE IF NOT EXISTS asset_meta (
			asset_id  INTEGER NOT NULL REFERENCES assets(id) ON DELETE CASCADE,
			key       TEXT    NOT NULL,
			value     TEXT    NOT NULL
		)
	)"),
	// --- edges ---
	TEXT(R"(
		CREATE TABLE IF NOT EXISTS edges (
			id             INTEGER PRIMARY KEY AUTOINCREMENT,
			from_id        INTEGER NOT NULL REFERENCES assets(id) ON DELETE CASCADE,
			to_id          INTEGER NOT NULL REFERENCES assets(id) ON DELETE CASCADE,
			rel            TEXT    NOT NULL,
			property_path  TEXT    DEFAULT '',
			dep_hardness   TEXT    DEFAULT ''
		)
	)"),
	// --- functions ---
	TEXT(R"(
		CREATE TABLE IF NOT EXISTS functions (
			id          INTEGER PRIMARY KEY AUTOINCREMENT,
			asset_id    INTEGER NOT NULL REFERENCES assets(id) ON DELETE CASCADE,
			name        TEXT    NOT NULL,
			category    TEXT,
			flags       TEXT,
			signature   TEXT,
			graph_json  TEXT,
			summary     TEXT,
			graph_hash  TEXT
		)
	)"),
	// --- graph_nodes (v3 placeholder) ---
	TEXT(R"(
		CREATE TABLE IF NOT EXISTS graph_nodes (
			id          INTEGER PRIMARY KEY AUTOINCREMENT,
			function_id INTEGER NOT NULL REFERENCES functions(id) ON DELETE CASCADE,
			node_guid   TEXT    NOT NULL,
			node_type   TEXT    NOT NULL,
			target      TEXT,
			comment     TEXT
		)
	)"),
	// --- graph_edges (v3 placeholder) ---
	TEXT(R"(
		CREATE TABLE IF NOT EXISTS graph_edges (
			function_id INTEGER NOT NULL REFERENCES functions(id) ON DELETE CASCADE,
			from_node   TEXT    NOT NULL,
			from_pin    TEXT    NOT NULL,
			to_node     TEXT    NOT NULL,
			to_pin      TEXT    NOT NULL
		)
	)"),
	// --- dt_rows ---
	TEXT(R"(
		CREATE TABLE IF NOT EXISTS dt_rows (
			id        INTEGER PRIMARY KEY AUTOINCREMENT,
			asset_id  INTEGER NOT NULL REFERENCES assets(id) ON DELETE CASCADE,
			row_name  TEXT    NOT NULL,
			row_json  TEXT    NOT NULL
		)
	)"),
	nullptr // sentinel
};

static const TCHAR* GCreateIndexesSQL[] = {
	TEXT("CREATE INDEX IF NOT EXISTS idx_assets_path     ON assets(path)"),
	TEXT("CREATE INDEX IF NOT EXISTS idx_assets_type     ON assets(asset_type)"),
	TEXT("CREATE INDEX IF NOT EXISTS idx_assets_parent   ON assets(parent)"),
	TEXT("CREATE INDEX IF NOT EXISTS idx_meta_kv         ON asset_meta(key, value)"),
	TEXT("CREATE INDEX IF NOT EXISTS idx_meta_asset      ON asset_meta(asset_id)"),
	TEXT("CREATE UNIQUE INDEX IF NOT EXISTS idx_edges_unique ON edges(from_id, to_id, rel, property_path)"),
	TEXT("CREATE INDEX IF NOT EXISTS idx_edges_from      ON edges(from_id, rel)"),
	TEXT("CREATE INDEX IF NOT EXISTS idx_edges_to        ON edges(to_id, rel)"),
	TEXT("CREATE INDEX IF NOT EXISTS idx_edges_rel       ON edges(rel)"),
	TEXT("CREATE INDEX IF NOT EXISTS idx_functions_asset ON functions(asset_id)"),
	TEXT("CREATE INDEX IF NOT EXISTS idx_graph_func      ON graph_nodes(function_id)"),
	TEXT("CREATE INDEX IF NOT EXISTS idx_graph_target    ON graph_nodes(target)"),
	TEXT("CREATE INDEX IF NOT EXISTS idx_dt_rows_asset   ON dt_rows(asset_id)"),
	TEXT("CREATE INDEX IF NOT EXISTS idx_dt_rows_name    ON dt_rows(asset_id, row_name)"),
	nullptr // sentinel
};

// ============================================================
// Construction / Destruction
// ============================================================

FMCPDatabase::FMCPDatabase()
{
}

FMCPDatabase::~FMCPDatabase()
{
	Close();
}

// ============================================================
// Open / Close
// ============================================================

bool FMCPDatabase::Open(const FString& DbPath)
{
	Close();

	// Ensure directory exists
	FString Dir = FPaths::GetPath(DbPath);
	IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
	if (!PlatformFile.DirectoryExists(*Dir))
	{
		PlatformFile.CreateDirectoryTree(*Dir);
	}

	Db = MakeUnique<FSQLiteDatabase>();
	if (!Db->Open(*DbPath, ESQLiteDatabaseOpenMode::ReadWriteCreate))
	{
		UE_LOG(LogMCPDatabase, Error, TEXT("Failed to open SQLite database: %s. Error: %s"), *DbPath, *Db->GetLastError());
		Db.Reset();
		return false;
	}

	// Enable WAL mode for concurrent reads
	Db->Execute(TEXT("PRAGMA journal_mode=WAL"));
	// Enable foreign keys
	Db->Execute(TEXT("PRAGMA foreign_keys=ON"));

	UE_LOG(LogMCPDatabase, Log, TEXT("Opened SQLite database: %s"), *DbPath);
	return true;
}

void FMCPDatabase::Close()
{
	if (Db)
	{
		Db->Close();
		Db.Reset();
	}
	PathToIdCache.Empty();
	bCacheBuilt = false;
}

bool FMCPDatabase::IsOpen() const
{
	return Db.IsValid() && Db->IsValid();
}

// ============================================================
// Schema
// ============================================================

bool FMCPDatabase::CreateSchema()
{
	if (!IsOpen()) return false;

	for (int32 i = 0; GCreateSchemaSQL[i] != nullptr; ++i)
	{
		if (!Db->Execute(GCreateSchemaSQL[i]))
		{
			UE_LOG(LogMCPDatabase, Error, TEXT("Failed to create table [%d]: %s"), i, *Db->GetLastError());
			return false;
		}
	}

	for (int32 i = 0; GCreateIndexesSQL[i] != nullptr; ++i)
	{
		if (!Db->Execute(GCreateIndexesSQL[i]))
		{
			UE_LOG(LogMCPDatabase, Error, TEXT("Failed to create index [%d]: %s"), i, *Db->GetLastError());
			return false;
		}
	}

	// --- Schema migration: edges v2 (property_path + dep_hardness) ---
	// Check if edges table has the new columns; if not, migrate
	{
		bool bHasPropertyPath = false;
		Db->Execute(TEXT("PRAGMA table_info(edges)"), [&](const FSQLitePreparedStatement& Row) -> ESQLitePreparedStatementExecuteRowResult
		{
			FString ColName;
			Row.GetColumnValueByIndex(1, ColName);
			if (ColName == TEXT("property_path")) bHasPropertyPath = true;
			return ESQLitePreparedStatementExecuteRowResult::Continue;
		});

		if (!bHasPropertyPath)
		{
			UE_LOG(LogMCPDatabase, Log, TEXT("Migrating edges table: adding property_path and dep_hardness columns"));
			Db->Execute(TEXT("ALTER TABLE edges ADD COLUMN property_path TEXT DEFAULT ''"));
			Db->Execute(TEXT("ALTER TABLE edges ADD COLUMN dep_hardness TEXT DEFAULT ''"));
			// Clear existing edges so the new unique index can be created
			// (edges will be rebuilt at startup by BuildAllEdges)
			Db->Execute(TEXT("DELETE FROM edges"));
		}
	}

	UE_LOG(LogMCPDatabase, Log, TEXT("Database schema created/verified"));
	return true;
}

// ============================================================
// Transactions
// ============================================================

void FMCPDatabase::BeginTransaction()
{
	if (IsOpen()) Db->Execute(TEXT("BEGIN TRANSACTION"));
}

void FMCPDatabase::CommitTransaction()
{
	if (IsOpen()) Db->Execute(TEXT("COMMIT"));
}

void FMCPDatabase::RollbackTransaction()
{
	if (IsOpen()) Db->Execute(TEXT("ROLLBACK"));
}

// ============================================================
// Assets
// ============================================================

int64 FMCPDatabase::UpsertAsset(const FMCPAssetRecord& Record)
{
	if (!IsOpen()) return -1;

	static const TCHAR* Sql = TEXT(R"(
		INSERT INTO assets (name, path, asset_type, parent, folder, modified_at, header_hash, row_struct, row_count, da_class)
		VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
		ON CONFLICT(path) DO UPDATE SET
			name=excluded.name, asset_type=excluded.asset_type, parent=excluded.parent,
			folder=excluded.folder, modified_at=excluded.modified_at, header_hash=excluded.header_hash,
			row_struct=excluded.row_struct, row_count=excluded.row_count, da_class=excluded.da_class
	)");

	FSQLitePreparedStatement Stmt = Db->PrepareStatement(Sql, ESQLitePreparedStatementFlags::Persistent);
	if (!Stmt.IsValid())
	{
		UE_LOG(LogMCPDatabase, Error, TEXT("UpsertAsset: Failed to prepare: %s"), *Db->GetLastError());
		return -1;
	}

	Stmt.SetBindingValueByIndex(1, Record.Name);
	Stmt.SetBindingValueByIndex(2, Record.Path);
	Stmt.SetBindingValueByIndex(3, Record.AssetType);
	Stmt.SetBindingValueByIndex(4, Record.Parent);
	Stmt.SetBindingValueByIndex(5, Record.Folder);
	Stmt.SetBindingValueByIndex(6, Record.ModifiedAt);
	Stmt.SetBindingValueByIndex(7, Record.HeaderHash);
	Stmt.SetBindingValueByIndex(8, Record.RowStruct);
	Stmt.SetBindingValueByIndex(9, (int64)Record.RowCount);
	Stmt.SetBindingValueByIndex(10, Record.DaClass);

	if (!Stmt.Execute())
	{
		UE_LOG(LogMCPDatabase, Error, TEXT("UpsertAsset failed for %s: %s"), *Record.Path, *Db->GetLastError());
		return -1;
	}

	int64 Id = Db->GetLastInsertRowId();

	// If ON CONFLICT updated (not inserted), last_insert_rowid may not reflect it; fetch by path
	if (Id <= 0)
	{
		Id = GetAssetId(Record.Path);
	}
	else
	{
		PathToIdCache.Add(Record.Path, Id);
	}

	return Id;
}

void FMCPDatabase::DeleteAsset(const FString& Path)
{
	if (!IsOpen()) return;

	FSQLitePreparedStatement Stmt = Db->PrepareStatement(TEXT("DELETE FROM assets WHERE path = ?"));
	if (Stmt.IsValid())
	{
		Stmt.SetBindingValueByIndex(1, Path);
		Stmt.Execute();
	}
	PathToIdCache.Remove(Path);
}

TOptional<FMCPAssetRecord> FMCPDatabase::GetAsset(const FString& Path)
{
	if (!IsOpen()) return {};

	FSQLitePreparedStatement Stmt = Db->PrepareStatement(
		TEXT("SELECT id, name, path, asset_type, parent, folder, modified_at, header_hash, row_struct, row_count, da_class FROM assets WHERE path = ?"));
	if (!Stmt.IsValid()) return {};

	Stmt.SetBindingValueByIndex(1, Path);

	TOptional<FMCPAssetRecord> Result;
	Stmt.Execute([&](const FSQLitePreparedStatement& Row) -> ESQLitePreparedStatementExecuteRowResult
	{
		Result = RowToAssetRecord(Row);
		return ESQLitePreparedStatementExecuteRowResult::Stop;
	});

	return Result;
}

int64 FMCPDatabase::GetAssetId(const FString& Path)
{
	// Check cache first
	if (const int64* Cached = PathToIdCache.Find(Path))
	{
		return *Cached;
	}

	if (!IsOpen()) return -1;

	FSQLitePreparedStatement Stmt = Db->PrepareStatement(TEXT("SELECT id FROM assets WHERE path = ?"));
	if (!Stmt.IsValid()) return -1;

	Stmt.SetBindingValueByIndex(1, Path);

	int64 Id = -1;
	Stmt.Execute([&](const FSQLitePreparedStatement& Row) -> ESQLitePreparedStatementExecuteRowResult
	{
		Row.GetColumnValueByIndex(0, Id);
		return ESQLitePreparedStatementExecuteRowResult::Stop;
	});

	if (Id > 0)
	{
		PathToIdCache.Add(Path, Id);
	}
	return Id;
}

int64 FMCPDatabase::GetAssetIdFresh(const FString& Path)
{
	// Always queries DB — use after long-running operations (e.g. asset loads) that
	// may have triggered a reindex, which can change IDs and invalidate PathToIdCache.
	if (!IsOpen()) return -1;

	FSQLitePreparedStatement Stmt = Db->PrepareStatement(TEXT("SELECT id FROM assets WHERE path = ?"));
	if (!Stmt.IsValid()) return -1;

	Stmt.SetBindingValueByIndex(1, Path);

	int64 Id = -1;
	Stmt.Execute([&](const FSQLitePreparedStatement& Row) -> ESQLitePreparedStatementExecuteRowResult
	{
		Row.GetColumnValueByIndex(0, Id);
		return ESQLitePreparedStatementExecuteRowResult::Stop;
	});

	if (Id > 0)
	{
		PathToIdCache.Add(Path, Id); // refresh cache with live value
	}
	return Id;
}

FString FMCPDatabase::GetAssetType(int64 AssetId)
{
	if (!IsOpen()) return TEXT("");

	FSQLitePreparedStatement Stmt = Db->PrepareStatement(TEXT("SELECT asset_type FROM assets WHERE id = ?"));
	if (!Stmt.IsValid()) return TEXT("");

	Stmt.SetBindingValueByIndex(1, AssetId);

	FString Type;
	Stmt.Execute([&](const FSQLitePreparedStatement& Row) -> ESQLitePreparedStatementExecuteRowResult
	{
		Row.GetColumnValueByIndex(0, Type);
		return ESQLitePreparedStatementExecuteRowResult::Stop;
	});
	return Type;
}

// ============================================================
// Meta
// ============================================================

void FMCPDatabase::SetMeta(int64 AssetId, const TArray<TPair<FString, FString>>& KeyValues)
{
	if (!IsOpen() || AssetId <= 0) return;

	FSQLitePreparedStatement Stmt = Db->PrepareStatement(
		TEXT("INSERT INTO asset_meta (asset_id, key, value) VALUES (?, ?, ?)"),
		ESQLitePreparedStatementFlags::Persistent);
	if (!Stmt.IsValid()) return;

	for (const auto& KV : KeyValues)
	{
		Stmt.Reset();
		Stmt.ClearBindings();
		Stmt.SetBindingValueByIndex(1, AssetId);
		Stmt.SetBindingValueByIndex(2, KV.Key);
		Stmt.SetBindingValueByIndex(3, KV.Value);
		Stmt.Execute();
	}
}

void FMCPDatabase::ClearMeta(int64 AssetId)
{
	if (!IsOpen()) return;

	FSQLitePreparedStatement Stmt = Db->PrepareStatement(TEXT("DELETE FROM asset_meta WHERE asset_id = ?"));
	if (Stmt.IsValid())
	{
		Stmt.SetBindingValueByIndex(1, AssetId);
		Stmt.Execute();
	}
}

TArray<FString> FMCPDatabase::GetMeta(int64 AssetId, const FString& Key)
{
	TArray<FString> Results;
	if (!IsOpen()) return Results;

	FSQLitePreparedStatement Stmt = Db->PrepareStatement(
		TEXT("SELECT value FROM asset_meta WHERE asset_id = ? AND key = ?"));
	if (!Stmt.IsValid()) return Results;

	Stmt.SetBindingValueByIndex(1, AssetId);
	Stmt.SetBindingValueByIndex(2, Key);

	Stmt.Execute([&](const FSQLitePreparedStatement& Row) -> ESQLitePreparedStatementExecuteRowResult
	{
		FString Val;
		Row.GetColumnValueByIndex(0, Val);
		Results.Add(MoveTemp(Val));
		return ESQLitePreparedStatementExecuteRowResult::Continue;
	});
	return Results;
}

// ============================================================
// Edges
// ============================================================

void FMCPDatabase::UpsertEdge(int64 FromId, int64 ToId, const FString& Rel, const FString& PropertyPath, const FString& DepHardness)
{
	if (!IsOpen() || FromId <= 0 || ToId <= 0) return;

	FSQLitePreparedStatement Stmt = Db->PrepareStatement(
		TEXT("INSERT INTO edges (from_id, to_id, rel, property_path, dep_hardness) VALUES (?, ?, ?, ?, ?)")
		TEXT(" ON CONFLICT(from_id, to_id, rel, property_path) DO UPDATE SET dep_hardness=excluded.dep_hardness"));
	if (!Stmt.IsValid()) return;

	Stmt.SetBindingValueByIndex(1, FromId);
	Stmt.SetBindingValueByIndex(2, ToId);
	Stmt.SetBindingValueByIndex(3, Rel);
	Stmt.SetBindingValueByIndex(4, PropertyPath);
	Stmt.SetBindingValueByIndex(5, DepHardness);
	Stmt.Execute();
}

void FMCPDatabase::ClearEdgesFrom(int64 AssetId)
{
	if (!IsOpen()) return;

	FSQLitePreparedStatement Stmt = Db->PrepareStatement(TEXT("DELETE FROM edges WHERE from_id = ?"));
	if (Stmt.IsValid())
	{
		Stmt.SetBindingValueByIndex(1, AssetId);
		Stmt.Execute();
	}
}

// ============================================================
// Functions
// ============================================================

int64 FMCPDatabase::UpsertFunction(int64 AssetId, const FMCPFunctionRecord& Func)
{
	if (!IsOpen() || AssetId <= 0) return -1;

	FSQLitePreparedStatement Stmt = Db->PrepareStatement(
		TEXT("INSERT INTO functions (asset_id, name, category, flags, signature, graph_hash) VALUES (?, ?, ?, ?, ?, ?)"));
	if (!Stmt.IsValid()) return -1;

	Stmt.SetBindingValueByIndex(1, AssetId);
	Stmt.SetBindingValueByIndex(2, Func.Name);
	Stmt.SetBindingValueByIndex(3, Func.Category);
	Stmt.SetBindingValueByIndex(4, Func.FlagsJson);
	Stmt.SetBindingValueByIndex(5, Func.SignatureJson);
	Stmt.SetBindingValueByIndex(6, Func.GraphHash);

	if (!Stmt.Execute()) return -1;
	return Db->GetLastInsertRowId();
}

void FMCPDatabase::ClearFunctions(int64 AssetId)
{
	if (!IsOpen()) return;

	FSQLitePreparedStatement Stmt = Db->PrepareStatement(TEXT("DELETE FROM functions WHERE asset_id = ?"));
	if (Stmt.IsValid())
	{
		Stmt.SetBindingValueByIndex(1, AssetId);
		Stmt.Execute();
	}
}

// ============================================================
// DataTable Rows
// ============================================================

int32 FMCPDatabase::UpsertDTRows(int64 AssetId, const TArray<TPair<FString, FString>>& Rows)
{
	if (!IsOpen() || AssetId <= 0) return 0;

	int32 Inserted = 0;
	for (const auto& Row : Rows)
	{
		FSQLitePreparedStatement Stmt = Db->PrepareStatement(
			TEXT("INSERT INTO dt_rows (asset_id, row_name, row_json) VALUES (?, ?, ?)"));
		if (!Stmt.IsValid()) continue;

		Stmt.SetBindingValueByIndex(1, AssetId);
		Stmt.SetBindingValueByIndex(2, Row.Key);
		Stmt.SetBindingValueByIndex(3, Row.Value);
		if (Stmt.Execute())
		{
			++Inserted;
		}
		else
		{
			UE_LOG(LogMCPDatabase, Error, TEXT("UpsertDTRows INSERT failed for row '%s': %s"), *Row.Key, *Db->GetLastError());
		}
	}
	return Inserted;
}

void FMCPDatabase::ClearDTRows(int64 AssetId)
{
	if (!IsOpen()) return;

	FSQLitePreparedStatement Stmt = Db->PrepareStatement(TEXT("DELETE FROM dt_rows WHERE asset_id = ?"));
	if (Stmt.IsValid())
	{
		Stmt.SetBindingValueByIndex(1, AssetId);
		Stmt.Execute();
	}
}

bool FMCPDatabase::HasDTRows(int64 AssetId)
{
	if (!IsOpen()) return false;

	FSQLitePreparedStatement Stmt = Db->PrepareStatement(TEXT("SELECT COUNT(*) FROM dt_rows WHERE asset_id = ?"));
	if (!Stmt.IsValid()) return false;

	Stmt.SetBindingValueByIndex(1, AssetId);

	int64 Count = 0;
	Stmt.Execute([&](const FSQLitePreparedStatement& Row) -> ESQLitePreparedStatementExecuteRowResult
	{
		Row.GetColumnValueByIndex(0, Count);
		return ESQLitePreparedStatementExecuteRowResult::Stop;
	});
	return Count > 0;
}

void FMCPDatabase::UpdateRowCount(int64 AssetId, int32 NewCount)
{
	if (!IsOpen()) return;

	FSQLitePreparedStatement Stmt = Db->PrepareStatement(TEXT("UPDATE assets SET row_count = ? WHERE id = ?"));
	if (Stmt.IsValid())
	{
		Stmt.SetBindingValueByIndex(1, (int64)NewCount);
		Stmt.SetBindingValueByIndex(2, AssetId);
		Stmt.Execute();
	}
}

TArray<TPair<FString, FString>> FMCPDatabase::GetDTRows(int64 AssetId, const TArray<FString>& RowNames, int32 Limit)
{
	TArray<TPair<FString, FString>> Results;
	if (!IsOpen()) return Results;

	if (RowNames.Num() > 0)
	{
		// Build IN clause
		FString Placeholders;
		for (int32 i = 0; i < RowNames.Num(); ++i)
		{
			if (i > 0) Placeholders += TEXT(", ");
			Placeholders += TEXT("?");
		}
		FString Sql = FString::Printf(TEXT("SELECT row_name, row_json FROM dt_rows WHERE asset_id = ? AND row_name IN (%s)"), *Placeholders);
		FSQLitePreparedStatement Stmt = Db->PrepareStatement(*Sql);
		if (!Stmt.IsValid()) return Results;

		Stmt.SetBindingValueByIndex(1, AssetId);
		for (int32 i = 0; i < RowNames.Num(); ++i)
		{
			Stmt.SetBindingValueByIndex(i + 2, RowNames[i]);
		}

		Stmt.Execute([&](const FSQLitePreparedStatement& Row) -> ESQLitePreparedStatementExecuteRowResult
		{
			FString Name, Json;
			Row.GetColumnValueByIndex(0, Name);
			Row.GetColumnValueByIndex(1, Json);
			Results.Emplace(MoveTemp(Name), MoveTemp(Json));
			return ESQLitePreparedStatementExecuteRowResult::Continue;
		});
	}
	else
	{
		// Return first N rows
		FSQLitePreparedStatement Stmt = Db->PrepareStatement(
			TEXT("SELECT row_name, row_json FROM dt_rows WHERE asset_id = ? LIMIT ?"));
		if (!Stmt.IsValid()) return Results;

		Stmt.SetBindingValueByIndex(1, AssetId);
		Stmt.SetBindingValueByIndex(2, (int64)Limit);

		Stmt.Execute([&](const FSQLitePreparedStatement& Row) -> ESQLitePreparedStatementExecuteRowResult
		{
			FString Name, Json;
			Row.GetColumnValueByIndex(0, Name);
			Row.GetColumnValueByIndex(1, Json);
			Results.Emplace(MoveTemp(Name), MoveTemp(Json));
			return ESQLitePreparedStatementExecuteRowResult::Continue;
		});
	}

	return Results;
}

int32 FMCPDatabase::GetDTRowCount(int64 AssetId)
{
	if (!IsOpen()) return 0;

	FSQLitePreparedStatement Stmt = Db->PrepareStatement(TEXT("SELECT COUNT(*) FROM dt_rows WHERE asset_id = ?"));
	if (!Stmt.IsValid()) return 0;

	Stmt.SetBindingValueByIndex(1, AssetId);

	int64 Count = 0;
	Stmt.Execute([&](const FSQLitePreparedStatement& Row) -> ESQLitePreparedStatementExecuteRowResult
	{
		Row.GetColumnValueByIndex(0, Count);
		return ESQLitePreparedStatementExecuteRowResult::Stop;
	});
	return (int32)Count;
}

// ============================================================
// Queries
// ============================================================

TArray<FMCPAssetRecord> FMCPDatabase::QueryAssetsByType(const FString& AssetType)
{
	TArray<FMCPAssetRecord> Results;
	if (!IsOpen()) return Results;

	FSQLitePreparedStatement Stmt = Db->PrepareStatement(
		TEXT("SELECT id, name, path, asset_type, parent, folder, modified_at, header_hash, row_struct, row_count, da_class FROM assets WHERE asset_type = ?"));
	if (!Stmt.IsValid()) return Results;

	Stmt.SetBindingValueByIndex(1, AssetType);

	Stmt.Execute([&](const FSQLitePreparedStatement& Row) -> ESQLitePreparedStatementExecuteRowResult
	{
		Results.Add(RowToAssetRecord(Row));
		return ESQLitePreparedStatementExecuteRowResult::Continue;
	});
	return Results;
}

TArray<FMCPAssetRecord> FMCPDatabase::QueryAllAssets()
{
	TArray<FMCPAssetRecord> Results;
	if (!IsOpen()) return Results;

	FSQLitePreparedStatement Stmt = Db->PrepareStatement(
		TEXT("SELECT id, name, path, asset_type, parent, folder, modified_at, header_hash, row_struct, row_count, da_class FROM assets"));
	if (!Stmt.IsValid()) return Results;

	Stmt.Execute([&](const FSQLitePreparedStatement& Row) -> ESQLitePreparedStatementExecuteRowResult
	{
		Results.Add(RowToAssetRecord(Row));
		return ESQLitePreparedStatementExecuteRowResult::Continue;
	});
	return Results;
}

TArray<FMCPRelatedResult> FMCPDatabase::GetRelated(int64 AssetId, const FString& Rel, bool bIncoming, bool bRecursive, int32 MaxDepth, int32 Limit, int32 Offset)
{
	TArray<FMCPRelatedResult> Results;
	if (!IsOpen() || AssetId <= 0) return Results;

	FString Sql;

	if (bRecursive)
	{
		// Recursive CTE for graph traversal
		if (bIncoming)
		{
			Sql = FString::Printf(TEXT(R"(
				WITH RECURSIVE tree AS (
					SELECT a.id, a.name, a.path, a.asset_type, e.rel, 1 as depth, e.property_path, e.dep_hardness
					FROM assets a
					JOIN edges e ON a.id = e.from_id
					WHERE e.to_id = ? %s

					UNION ALL

					SELECT a.id, a.name, a.path, a.asset_type, e.rel, t.depth + 1, e.property_path, e.dep_hardness
					FROM assets a
					JOIN edges e ON a.id = e.from_id
					JOIN tree t ON e.to_id = t.id
					WHERE t.depth < ? %s
				)
				SELECT DISTINCT id, name, path, asset_type, rel, depth, property_path, dep_hardness FROM tree ORDER BY depth LIMIT ? OFFSET ?
			)"),
				Rel != TEXT("all") ? *FString::Printf(TEXT("AND e.rel = '%s'"), *Rel) : TEXT(""),
				Rel != TEXT("all") ? *FString::Printf(TEXT("AND e.rel = '%s'"), *Rel) : TEXT(""));
		}
		else
		{
			Sql = FString::Printf(TEXT(R"(
				WITH RECURSIVE tree AS (
					SELECT a.id, a.name, a.path, a.asset_type, e.rel, 1 as depth, e.property_path, e.dep_hardness
					FROM assets a
					JOIN edges e ON a.id = e.to_id
					WHERE e.from_id = ? %s

					UNION ALL

					SELECT a.id, a.name, a.path, a.asset_type, e.rel, t.depth + 1, e.property_path, e.dep_hardness
					FROM assets a
					JOIN edges e ON a.id = e.to_id
					JOIN tree t ON e.from_id = t.id
					WHERE t.depth < ? %s
				)
				SELECT DISTINCT id, name, path, asset_type, rel, depth, property_path, dep_hardness FROM tree ORDER BY depth LIMIT ? OFFSET ?
			)"),
				Rel != TEXT("all") ? *FString::Printf(TEXT("AND e.rel = '%s'"), *Rel) : TEXT(""),
				Rel != TEXT("all") ? *FString::Printf(TEXT("AND e.rel = '%s'"), *Rel) : TEXT(""));
		}
	}
	else
	{
		// Simple one-hop query
		if (bIncoming)
		{
			Sql = FString::Printf(TEXT(R"(
				SELECT a.id, a.name, a.path, a.asset_type, e.rel, 1 as depth, e.property_path, e.dep_hardness
				FROM assets a
				JOIN edges e ON a.id = e.from_id
				WHERE e.to_id = ? %s
				LIMIT ? OFFSET ?
			)"),
				Rel != TEXT("all") ? *FString::Printf(TEXT("AND e.rel = '%s'"), *Rel) : TEXT(""));
		}
		else
		{
			Sql = FString::Printf(TEXT(R"(
				SELECT a.id, a.name, a.path, a.asset_type, e.rel, 1 as depth, e.property_path, e.dep_hardness
				FROM assets a
				JOIN edges e ON a.id = e.to_id
				WHERE e.from_id = ? %s
				LIMIT ? OFFSET ?
			)"),
				Rel != TEXT("all") ? *FString::Printf(TEXT("AND e.rel = '%s'"), *Rel) : TEXT(""));
		}
	}

	FSQLitePreparedStatement Stmt = Db->PrepareStatement(*Sql);
	if (!Stmt.IsValid())
	{
		UE_LOG(LogMCPDatabase, Error, TEXT("GetRelated: Failed to prepare statement: %s"), *Db->GetLastError());
		return Results;
	}

	Stmt.SetBindingValueByIndex(1, AssetId);
	if (bRecursive)
	{
		Stmt.SetBindingValueByIndex(2, (int64)MaxDepth);
		Stmt.SetBindingValueByIndex(3, (int64)Limit);
		Stmt.SetBindingValueByIndex(4, (int64)Offset);
	}
	else
	{
		Stmt.SetBindingValueByIndex(2, (int64)Limit);
		Stmt.SetBindingValueByIndex(3, (int64)Offset);
	}

	Stmt.Execute([&](const FSQLitePreparedStatement& Row) -> ESQLitePreparedStatementExecuteRowResult
	{
		FMCPRelatedResult R;
		Row.GetColumnValueByIndex(1, R.Name);
		Row.GetColumnValueByIndex(2, R.Path);
		Row.GetColumnValueByIndex(3, R.AssetType);
		Row.GetColumnValueByIndex(4, R.Rel);
		int64 DepthVal = 1;
		Row.GetColumnValueByIndex(5, DepthVal);
		R.Depth = (int32)DepthVal;
		Row.GetColumnValueByIndex(6, R.PropertyPath);
		Row.GetColumnValueByIndex(7, R.DepHardness);
		Results.Add(MoveTemp(R));
		return ESQLitePreparedStatementExecuteRowResult::Continue;
	});

	return Results;
}

// ============================================================
// Raw SQL
// ============================================================

bool FMCPDatabase::Execute(const FString& Sql)
{
	if (!IsOpen()) return false;
	return Db->Execute(*Sql);
}

int64 FMCPDatabase::ExecuteWithCallback(const FString& Sql, TFunction<void(const FSQLitePreparedStatement&)> RowCallback)
{
	if (!IsOpen()) return INDEX_NONE;

	return Db->Execute(*Sql, [&](const FSQLitePreparedStatement& Row) -> ESQLitePreparedStatementExecuteRowResult
	{
		RowCallback(Row);
		return ESQLitePreparedStatementExecuteRowResult::Continue;
	});
}

// ============================================================
// Stats
// ============================================================

int32 FMCPDatabase::GetAssetCount()
{
	if (!IsOpen()) return 0;

	int64 Count = 0;
	Db->Execute(TEXT("SELECT COUNT(*) FROM assets"), [&](const FSQLitePreparedStatement& Row) -> ESQLitePreparedStatementExecuteRowResult
	{
		Row.GetColumnValueByIndex(0, Count);
		return ESQLitePreparedStatementExecuteRowResult::Stop;
	});
	return (int32)Count;
}

int32 FMCPDatabase::GetAssetCountByType(const FString& AssetType)
{
	if (!IsOpen()) return 0;

	FSQLitePreparedStatement Stmt = Db->PrepareStatement(TEXT("SELECT COUNT(*) FROM assets WHERE asset_type = ?"));
	if (!Stmt.IsValid()) return 0;

	Stmt.SetBindingValueByIndex(1, AssetType);

	int64 Count = 0;
	Stmt.Execute([&](const FSQLitePreparedStatement& Row) -> ESQLitePreparedStatementExecuteRowResult
	{
		Row.GetColumnValueByIndex(0, Count);
		return ESQLitePreparedStatementExecuteRowResult::Stop;
	});
	return (int32)Count;
}

int32 FMCPDatabase::GetEdgeCount()
{
	if (!IsOpen()) return 0;

	int64 Count = 0;
	Db->Execute(TEXT("SELECT COUNT(*) FROM edges"), [&](const FSQLitePreparedStatement& Row) -> ESQLitePreparedStatementExecuteRowResult
	{
		Row.GetColumnValueByIndex(0, Count);
		return ESQLitePreparedStatementExecuteRowResult::Stop;
	});
	return (int32)Count;
}

// ============================================================
// Helpers
// ============================================================

void FMCPDatabase::InvalidatePathCache(const FString& Path)
{
	PathToIdCache.Remove(Path);
}

void FMCPDatabase::RebuildPathCache()
{
	PathToIdCache.Empty();
	if (!IsOpen()) return;

	Db->Execute(TEXT("SELECT id, path FROM assets"), [&](const FSQLitePreparedStatement& Row) -> ESQLitePreparedStatementExecuteRowResult
	{
		int64 Id;
		FString Path;
		Row.GetColumnValueByIndex(0, Id);
		Row.GetColumnValueByIndex(1, Path);
		PathToIdCache.Add(MoveTemp(Path), Id);
		return ESQLitePreparedStatementExecuteRowResult::Continue;
	});

	bCacheBuilt = true;
}

FMCPAssetRecord FMCPDatabase::RowToAssetRecord(const FSQLitePreparedStatement& Stmt) const
{
	FMCPAssetRecord R;
	Stmt.GetColumnValueByIndex(0, R.Id);
	Stmt.GetColumnValueByIndex(1, R.Name);
	Stmt.GetColumnValueByIndex(2, R.Path);
	Stmt.GetColumnValueByIndex(3, R.AssetType);
	Stmt.GetColumnValueByIndex(4, R.Parent);
	Stmt.GetColumnValueByIndex(5, R.Folder);
	Stmt.GetColumnValueByIndex(6, R.ModifiedAt);
	Stmt.GetColumnValueByIndex(7, R.HeaderHash);
	Stmt.GetColumnValueByIndex(8, R.RowStruct);

	int64 RowCountVal = 0;
	Stmt.GetColumnValueByIndex(9, RowCountVal);
	R.RowCount = (int32)RowCountVal;

	Stmt.GetColumnValueByIndex(10, R.DaClass);
	return R;
}
