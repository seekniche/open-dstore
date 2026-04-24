/*
 * Copyright (C) 2026 Huawei Technologies Co.,Ltd.
 *
 * dstore is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * dstore is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. if not, see <https://www.gnu.org/licenses/>.
 *
 * ---------------------------------------------------------------------------------------
 *
 * tool_wal_dump.cpp
 *
 * Description:
 * tools/waldump/src/tool_wal_dump.cpp
 *
 * ---------------------------------------------------------------------------------------
 *
 */
#include <unistd.h>
#include <getopt.h>

#include "wal/dstore_wal_dump_file_reader.h"
#include "wal/dstore_wal_dump.h"

#include "patch_control_checkpoint.h"

using namespace DSTORE;

/* Long-only option ids for --patch-control-checkpoint and friends. */
enum LongOnlyOpt : int {
    OPT_LONG_BASE = 1000,
    OPT_SCAN_VALID_CHECKPOINT,
    OPT_WAL_DIR,
    OPT_FROM_PLSN,
    OPT_PATCH_CONTROL_CHECKPOINT,
    OPT_CONTROL_FILE,
    OPT_DRY_RUN,
};

static struct PatchCliState {
    bool requested = false;
    const char *controlFile = nullptr;
    bool dryRun = false;
} g_patchCli;

static const char *g_progName = "waldump";

static RetStatus ParsePlsnRange(char *arg, WalDumpConfig &config);

static RetStatus ParsePageId(char *arg, WalDumpConfig &config);

static RetStatus ParseModuleFilter(char *arg, WalDumpConfig &config);

static RetStatus ParseTypeFilter(char *arg, WalDumpConfig &config);

static RetStatus ParseXidFilter(char *arg, WalDumpConfig &config);

static RetStatus ParseVfsName(char *arg, WalDumpConfig &config);

static void ResetPatchCliState()
{
    g_patchCli = {};
}

static RetStatus ParseSinglePlsn(const char *arg, uint64 &plsn)
{
    if (sscanf_s(arg, "%lu", &plsn) != 1 || plsn == 0) {
        (void)fprintf(stderr, "%s: could not parse \"%s\" as a valid plsn.\n", g_progName, arg);
        return DSTORE_FAIL;
    }
    return DSTORE_SUCC;
}

static int HandlePatchControlCheckpointCommand(const WalDumpConfig &config)
{
    if (!g_patchCli.requested) {
        return -1;
    }

    if (config.walId == INVALID_WAL_ID) {
        (void)fprintf(stderr, "%s: --patch-control-checkpoint requires --walid/--wal-id.\n", g_progName);
        return EXIT_FAILURE;
    }
    if (config.startPlsn == 0 || config.startPlsn == WAL_DUMP_INVALID_PLSN ||
        config.endPlsn != WAL_DUMP_INVALID_PLSN) {
        (void)fprintf(stderr, "%s: --patch-control-checkpoint requires a single --plsn value.\n", g_progName);
        return EXIT_FAILURE;
    }

    PatchControlCheckpointArgs args = {
        .controlFilePath = g_patchCli.controlFile,
        .walId = config.walId,
        .plsn = config.startPlsn,
        .dryRun = g_patchCli.dryRun,
    };
    return STORAGE_FUNC_SUCC(RunPatchControlCheckpoint(args)) ? EXIT_SUCCESS : EXIT_FAILURE;
}

static void Usage()
{
    printf(R"(
waldump parse and dump dstore transaction logs for debugging.

Usage:
waldump WAL_STORAGE_TYPE [WAL_LSN_RANGE_INFO] [OUTPUT_FILTER]
waldump OTHER

example1(dump all WalStreams in PageStore after plsn 10000):
    waldump -t 1 -g /PATH/tenant_gaussdb_start_config_1_1.json -p 10000
example2(dump WalStream whose walId is 1 in PageStore):
    waldump -t 1 -g /PATH/tenant_gaussdb_start_config_1_1.json -w 1
example3(dump wal in local directory):
    waldump -d /WAL_PATH/
example4(dump WalStream that belongs to template pdb in PageStore):
    waldump -t 1 -g /PATH/tenant_gaussdb_start_config_1_1.json -b test_tenant.vfs.template_cdb
example5(scan valid checkpoint candidates in a local wal dir):
    waldump --scan-valid-checkpoint --wal-dir /WAL_PATH/ [--from-plsn 12345]
example6(patch control file checkpoint for one wal stream):
    waldump --patch-control-checkpoint --control-file /PATH/database_control_1 --wal-id 1 --plsn 12345 [--dry-run]

WAL_STORAGE_TYPE:
  -t, --vfs_type=TYPE               Specify the vfs type: LocalFileSystem:0, PageStore:1
  -d, --dir=DIR                     On vfs_type = LocalFileSystem, use this param to specify directory in which
                                    to find wal files (default: ./)
  -g, --vfs_config=PATH             On vfs_type = LocalFileSystem, use this param to specify the vfs config to get
                                    wal file info
  -b, --pdb_id=PDB_ID               Only show wal records in the specified PDB_ID, only support gauss store and tenant
                                    isolation type
  -v, --vfs_name=NAME               Only show wal records in the specified VFS_NAME (default: root pdb)
  -i, --comm_local_ip=LOCAL_IP      On vfs_type = PageStore, use this param to specify the communication local ip
  -a, --comm_auth_type=VAL          On vfs_type = PageStore, use this param to specify the communication auth type
  -R, --comm_trd=TRD_MIN:TRD_MAX    Use this param to specify the communication thread min and max num

WAL_LSN_RANGE_INFO:
  -w, --walid=WAL_ID                Specify wal id of target wal stream
  -p, --plsn=START_PLSN[,END_PLSN]  Start reading wal position between START_PLSN and END_PLSN
                                    specify END_PLSN only by ",END_PLSN"
  -k, --after_checkpoint            Only dump wal after checkpoint diskrecovery plsn

OUTPUT_FILTER:
  -n, --limit=N                     Number of records to display per output
  -m, --module=NAME                 Only show records generated by module specified by NAME
                                    use --module=list to list valid module names
  -T, --type=TYPE                   Only show records of specified wal type
                                    wal type can be referenced by storage_wal_struct.h:enum WalType
  -x, --xid=ZONE_ID,LOGIC_SLOT_ID   Only show records with transaction id XID
  -P, --pageId=FILE_ID,BLOCK_ID     Only show records with pageId fileId, blockId
  -c, --check_page_error            Check page error when --pageId is set
  -D, --dumpDir=path                Path of the output file, (default: ./)

OTHER:
      --scan-valid-checkpoint       Scan local WAL files and print each valid group header plus
                                    the last valid group end plsn
      --wal-dir=DIR                 Local WAL directory used by --scan-valid-checkpoint
      --from-plsn=PLSN              Start scanning from the given PLSN (single value)
      --patch-control-checkpoint    Patch one WAL stream's latest checkpoint in a control file
      --control-file=PATH           Target control file path for --patch-control-checkpoint
      --dry-run                     Validate and print the patch without modifying the control file
  -h, --help                        Show this help and exit
  -V, --version                     Output version information and exit
)");
}

static void PrintModuleList()
{
    for (uint16 i = 0; i < MAX_MODULE_ID; i++) {
        printf("%s\n", MODULE_DESC_TABLE[i].name);
    }
}

static RetStatus ParseCmdLineInput(int argc, char **argv, WalDumpConfig &config)
{
    int option;
    int optIndex;
    const char *shortOptions = "w:p:s:n:m:t:x:d:v:g:f:kP:T:hVi:a:R:b:D:c";
    ResetPatchCliState();
    struct option longOptions[] = {
        {"vfs_type", required_argument, nullptr, 't'},
        {"dir", required_argument, nullptr, 'd'},
        {"wal-dir", required_argument, nullptr, OPT_WAL_DIR},
        {"vfs_config", required_argument, nullptr, 'g'},
        {"vfs_name", required_argument, nullptr, 'v'},
        {"walid", required_argument, nullptr, 'w'},
        {"wal-id", required_argument, nullptr, 'w'},
        {"plsn", required_argument, nullptr, 'p'},
        {"from-plsn", required_argument, nullptr, OPT_FROM_PLSN},
        {"after_checkpoint", no_argument, nullptr, 'k'},
        {"limit", required_argument, nullptr, 'n'},
        {"module", required_argument, nullptr, 'm'},
        {"type", required_argument, nullptr, 'T'},
        {"xid", required_argument, nullptr, 'x'},
        {"pageId", required_argument, nullptr, 'P'},
        {"help", no_argument, nullptr, 'h'},
        {"version", no_argument, nullptr, 'V'},
        {"comm_local_ip", required_argument, nullptr, 'i'},
        {"comm_auth_type", required_argument, nullptr, 'a'},
        {"comm_trd", required_argument, nullptr, 'R'},
        {"output path", required_argument, nullptr, 'D'},
        {"check_page_error", no_argument, nullptr, 'c'},
        {"scan-valid-checkpoint", no_argument, nullptr, OPT_SCAN_VALID_CHECKPOINT},
        {"patch-control-checkpoint", no_argument, nullptr, OPT_PATCH_CONTROL_CHECKPOINT},
        {"control-file", required_argument, nullptr, OPT_CONTROL_FILE},
        {"dry-run", no_argument, nullptr, OPT_DRY_RUN},
        {nullptr, 0, nullptr, 0}
    };
    while ((option = getopt_long(argc, argv, shortOptions, longOptions, &optIndex)) != -1) {
        switch (option) {
            case 't':
                VFSType vfsType;
                if (sscanf_s(optarg, "%hu", &vfsType) != 1) {
                    (void)fprintf(stderr, "%s: could not parse \"%s\" as a valid vfs type\n", g_progName, optarg);
                    return DSTORE_FAIL;
                }
                if (vfsType == VFSType::VFS_TYPE_LOCAL_FS) {
                    config.vfsType = StorageType::LOCAL;
                } else if (vfsType == VFSType::VFS_TYPE_PAGE_STORE) {
                    config.vfsType = StorageType::PAGESTORE;
                } else {
                    (void)fprintf(stderr, "%s: invalid vfs type \"%s\".\n", g_progName, optarg);
                    return DSTORE_FAIL;
                }
                break;
            case 'd':
                if (sprintf_s(config.dir, MAXPGPATH, "%s", optarg) == -1) {
                    (void)fprintf(stderr, "%s: sprintf dir name(%s) fail.\n", g_progName, optarg);
                    return DSTORE_FAIL;
                }
                break;
            case OPT_WAL_DIR:
                config.vfsType = StorageType::LOCAL;
                if (sprintf_s(config.dir, MAXPGPATH, "%s", optarg) == -1) {
                    (void)fprintf(stderr, "%s: sprintf wal-dir(%s) fail.\n", g_progName, optarg);
                    return DSTORE_FAIL;
                }
                break;
            case 'g':
                config.vfsConfigPath = strdup(optarg);
                break;
            case 'v':
                if (STORAGE_FUNC_FAIL(ParseVfsName(optarg, config))) {
                    return DSTORE_FAIL;
                }
                break;
            case 'w':
                if (sscanf_s(optarg, "%llu", &config.walId) != 1) {
                    (void)fprintf(stderr, "%s: could not parse walId \"%s\"\n", g_progName, optarg);
                    return DSTORE_FAIL;
                }
                break;
            case 'p':
                if (STORAGE_FUNC_FAIL(ParsePlsnRange(optarg, config))) {
                    return DSTORE_FAIL;
                }
                break;
            case OPT_FROM_PLSN:
                config.endPlsn = WAL_DUMP_INVALID_PLSN;
                if (STORAGE_FUNC_FAIL(ParseSinglePlsn(optarg, config.startPlsn))) {
                    return DSTORE_FAIL;
                }
                break;
            case 'k':
                config.dumpWalAfterCheckpoint = true;
                break;
            case 'n':
                if (sscanf_s(optarg, "%lu", &config.recordNumPerInputLimit) != 1) {
                    (void)fprintf(stderr, "%s: could not parse argument (limit) \"%s\"\n", g_progName, optarg);
                    return DSTORE_FAIL;
                }
                break;
            case 'm':
                if (strcmp(optarg, "list") == 0) {
                    PrintModuleList();
                    config.commandType = WalDumpCommandType::LIST_MODULE;
                    return DSTORE_SUCC;
                }
                if (STORAGE_FUNC_FAIL(ParseModuleFilter(optarg, config))) {
                    return DSTORE_FAIL;
                }
                break;
            case 'T':
                if (STORAGE_FUNC_FAIL(ParseTypeFilter(optarg, config))) {
                    return DSTORE_FAIL;
                }
                break;
            case 'x':
                if (STORAGE_FUNC_FAIL(ParseXidFilter(optarg, config))) {
                    return DSTORE_FAIL;
                }
                break;
            case 'P':
                if (STORAGE_FUNC_FAIL(ParsePageId(optarg, config))) {
                    return DSTORE_FAIL;
                }
                break;
            case 'h':
                Usage();
                config.commandType = WalDumpCommandType::HELP;
                return DSTORE_SUCC;
            case 'V':
                /**
                * 1.0: Basic version
                *   1.1: Add -w option to dump by walId on pagestore condition, and unite option with pagedump tool
                *   1.2: Support dump all WalStream's Wal
                *   1.3: Support dump wal after checkpoint by -k
                *   1.4: Not support dump by wal file number and fileId, because
                *        (1): vfs remove fileId
                *        (2): Wal change WalFileName from WAL_ID-TIMELINE-INDEX to WALID-TIMELINE-START_PLSN
                */
                printf("dstore waldump 1.4\n");
                config.commandType = WalDumpCommandType::VERSION;
                return DSTORE_SUCC;
            case 'a':
                if (sscanf_s(optarg, "%u", &config.commConfig.authType) != 1) {
                    (void)fprintf(stderr, "%s: could not parse communication auth type \"%s\"\n", g_progName, optarg);
                    return DSTORE_FAIL;
                }
                break;
            case 'i':
                config.commConfig.localIp = strdup(optarg);
                break;
            case 'R':
                if (STORAGE_FUNC_FAIL(PageDiagnose::ParseCommThreadNum(g_progName, optarg, &config.commConfig))) {
                    return DSTORE_FAIL;
                }
                break;
            case 'b':
                if (sscanf_s(optarg, "%u", &config.pdbId) != 1) {
                    (void)fprintf(stderr, "%s: could not parse pdb id \"%s\"\n", g_progName, optarg);
                    return DSTORE_FAIL;
                }
                if (config.pdbId == DSTORE::INVALID_PDB_ID) {
                    (void)fprintf(stderr, "%s: pdb id \"%s\" invalid\n", g_progName, optarg);
                    return DSTORE_FAIL;
                }
                break;
             case 'D':
                if (sprintf_s(config.dumpDir, MAXPGPATH, "%s", optarg) == -1) {
                    (void)fprintf(stderr, "%s: sprintf dumpDir name(%s) fail.\n", g_progName, optarg);
                    return DSTORE_FAIL;
                }
                break;
            case 'c':
                config.checkPageError = true;
                break;
            case OPT_SCAN_VALID_CHECKPOINT:
                config.scanValidCheckpoint = true;
                break;
            case OPT_PATCH_CONTROL_CHECKPOINT:
                g_patchCli.requested = true;
                break;
            case OPT_CONTROL_FILE:
                g_patchCli.controlFile = optarg;
                break;
            case OPT_DRY_RUN:
                g_patchCli.dryRun = true;
                break;
            default:
                return DSTORE_FAIL;
        }
    }
    if (optind != argc) {
        fprintf(stderr, "%s: input wrong command-line arguments numbers\n", g_progName);
        return DSTORE_FAIL;
    }
    return DSTORE_SUCC;
}

static RetStatus ParsePlsnRange(char *arg, WalDumpConfig &config)
{
    char *saveStr;
    char *startPlsnStr = strtok_r(arg, ",", &saveStr);
    char *endPlsnStr = strtok_r(nullptr, ",", &saveStr);
    if (startPlsnStr == nullptr) {
        (void)fprintf(stderr, "%s: \"%s\" invalid option format.\n", g_progName, arg);
        return DSTORE_FAIL;
    }
    if (optarg[0] == ',') {
        endPlsnStr = startPlsnStr;
        startPlsnStr = nullptr;
    }
    if (startPlsnStr != nullptr) {
        if (sscanf_s(startPlsnStr, "%lu", &config.startPlsn) != 1) {
            (void)fprintf(stderr, "%s: could not parse start plsn str \"%s\"\n", g_progName, startPlsnStr);
            return DSTORE_FAIL;
        }
    }
    if (config.startPlsn == 0) {
        (void)fprintf(stderr, "%s: could not designate start plsn to 0, default is 0. \n", g_progName);
        return DSTORE_FAIL;
    }
    if (endPlsnStr != nullptr) {
        if (sscanf_s(endPlsnStr, "%lu", &config.endPlsn) != 1) {
            (void)fprintf(stderr, "%s: could not parse end plsn str \"%s\"\n", g_progName, endPlsnStr);
            return DSTORE_FAIL;
        }
    }
    return DSTORE_SUCC;
}

static RetStatus ParseModuleFilter(char *arg, WalDumpConfig &config)
{
    for (uint16 i = 0; i <= MAX_MODULE_ID; i++) {
        if (strcmp(arg, MODULE_DESC_TABLE[i].name) == 0) {
            config.moduleFilter = i;
            break;
        }
    }
    if (config.moduleFilter == WAL_DUMP_INVALID_FILTER) {
        (void)fprintf(stderr, "%s: module name \"%s\" does not exist\n", g_progName, optarg);
        return DSTORE_FAIL;
    }
    return DSTORE_SUCC;
}

static RetStatus ParseTypeFilter(char *arg, WalDumpConfig &config)
{
    if (sscanf_s(arg, "%u", &config.typeFilter) != 1) {
        (void)fprintf(stderr, "%s: could not parse type filter number\"%s\"\n", g_progName, arg);
        return DSTORE_FAIL;
    }
    if (config.typeFilter >= static_cast<uint16>(WAL_TYPE_BUTTOM)) {
        (void)fprintf(stderr, "%s: Wal type(%u) does not exist\n", g_progName, config.typeFilter);
        return DSTORE_FAIL;
    }
    return DSTORE_SUCC;
}

static RetStatus ParseXidFilter(char *arg, WalDumpConfig &config)
{
    char *saveStr;
    char *zoneIdStr = strtok_r(arg, ",", &saveStr);
    char *logicSlotIdStr = strtok_r(nullptr, ",", &saveStr);
    if (zoneIdStr == nullptr || logicSlotIdStr == nullptr) {
        (void)fprintf(stderr, "%s: \"%s\" invalid option format.\n", g_progName, arg);
        return DSTORE_FAIL;
    }
    uint64 zoneId = 0;
    if (sscanf_s(zoneIdStr, "%lu", &zoneId) != 1) {
        (void)fprintf(stderr, "%s: could not parse Xid-zoneId str \"%s\"\n", g_progName, zoneIdStr);
        return DSTORE_FAIL;
    }
    uint64 logicSlotId = 0;
    if (sscanf_s(logicSlotIdStr, "%u", &logicSlotId) != 1) {
        (void)fprintf(stderr, "%s: could not parse Xid-logicSlotId str \"%s\"\n", g_progName, logicSlotIdStr);
        return DSTORE_FAIL;
    }
    config.xidFilter = {zoneId, logicSlotId};
    return DSTORE_SUCC;
}

static RetStatus ParsePageId(char *arg, WalDumpConfig &config)
{
    char *saveStr;
    char *fileIdStr = strtok_r(arg, ",", &saveStr);
    char *blockIdStr = strtok_r(nullptr, ",", &saveStr);
    if (fileIdStr == nullptr || blockIdStr == nullptr) {
        (void)fprintf(stderr, "%s: \"%s\" invalid option format.\n", g_progName, arg);
        return DSTORE_FAIL;
    }
    if (sscanf_s(fileIdStr, "%hu", &config.pageIdFilter.m_fileId) != 1) {
        (void)fprintf(stderr, "%s: could not parse pageId-fileId str \"%s\"\n", g_progName, fileIdStr);
        return DSTORE_FAIL;
    }
    uint32 blockId;
    if (sscanf_s(blockIdStr, "%u", &blockId) != 1) {
        (void)fprintf(stderr, "%s: could not parse pageId-blockId str \"%s\"\n", g_progName, blockIdStr);
        return DSTORE_FAIL;
    }
    config.pageIdFilter.m_blockId = blockId;
    return DSTORE_SUCC;
}

static RetStatus ParseVfsName(char *arg, WalDumpConfig &config)
{
    char *vfsName = strdup(arg);
    int rc = memcpy_s(config.pdbVfsName, DSTORE_VFS_NAME_MAX_LEN, vfsName, strlen(vfsName));
    free(vfsName);
    if (rc != 0) {
        (void)fprintf(stderr, "%s: could not parse vfsName, memcpy fail. \"%s\"\n", g_progName, optarg);
        return DSTORE_FAIL;
    }
    return DSTORE_SUCC;
}

static bool CheckWalLsnRangeInfo(WalDumpConfig &config)
{
    if (config.vfsType == StorageType::PAGESTORE) {
        if (config.vfsConfigPath == nullptr) {
            (void)fprintf(stderr, "%s: Must specify vfsConfigPath on pagestore.\n", g_progName);
            return false;
        }
    }
    return true;
}

static bool CheckVfsInfo(WalDumpConfig &config)
{
    if (config.pdbId != DSTORE::INVALID_PDB_ID && strlen(config.pdbVfsName) != 0) {
        (void)fprintf(stderr, "%s: Can not specify both pdbId and pdbVfsName.\n", g_progName);
        return false;
    }
    return true;
}

int main(int argc, char **argv)
{
    WalDumpConfig config;
    RetStatus retStatus = WalDumper::InitWalDumpConfig(config);
    if (STORAGE_FUNC_FAIL(retStatus)) {
        goto ERROR_EXIT;
    }
    retStatus = ParseCmdLineInput(argc, argv, config);
    if (STORAGE_FUNC_FAIL(retStatus)) {
        goto ERROR_EXIT;
    }
    {
        int patchExitCode = HandlePatchControlCheckpointCommand(config);
        if (patchExitCode >= 0) {
            free(config.vfsConfigPath);
            free(config.pdbVfsName);
            free(config.commConfig.localIp);
            return patchExitCode;
        }
    }
    if (config.commandType != WalDumpCommandType::DUMP_WAL_RECORD) {
        return EXIT_SUCCESS;
    }

    if (!CheckWalLsnRangeInfo(config) || !CheckVfsInfo(config)) {
        goto ERROR_EXIT;
    }

    if(config.checkPageError && config.pageIdFilter == INVALID_PAGE_ID) {
        fprintf(stderr, "-c needs to be used with -P");
        goto ERROR_EXIT;
    }

    WalDumper::DumpByConfig(&config);
    fflush(stdout);
    fflush(stderr);

    free(config.vfsConfigPath);
    free(config.pdbVfsName);
    free(config.commConfig.localIp);
    return EXIT_SUCCESS;

ERROR_EXIT:
    free(config.vfsConfigPath);
    free(config.pdbVfsName);
    free(config.commConfig.localIp);
    fprintf(stderr, "%s: some input arguments are invalid.\n"
                    "Try \"%s --help\" for more information.\n", g_progName, g_progName);
    fflush(stdout);
    fflush(stderr);
    return EXIT_FAILURE;
}
