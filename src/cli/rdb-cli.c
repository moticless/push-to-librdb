#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

/* Rely only on API (and not internal parser headers) */
#include "../../api/librdb-api.h"
#include "../../api/librdb-ext-api.h"

#define UNUSED(x) (void)(x)

FILE* logfile = NULL;
#define LOG_FILE_PATH_DEF "./rdb-cli.log"

/* common options to all FORMATTERS */
typedef struct Options {
    const char *logfilePath;
    RdbRes (*formatFunc)(RdbParser *p, char *input, int argc, char **argv);
} Options;

static int getOptArg(int argc, char* argv[], int *at,  char *abbrvOpt, char *opt, char *token, int *flag, const char **arg) {
    if ((strcmp(token, abbrvOpt) == 0) || (strcmp(token, opt) == 0)) {
        if (arg) {
            if ((*at) + 1 == argc) {
                fprintf(stderr, "%s (%s) requires one argument.", opt, abbrvOpt);
                exit(RDB_ERR_GENERAL);
            }
            *arg = argv[++(*at)];
        }
        if (flag) *flag = 1;
        return 1;
    } else {
        return 0;
    }
}

static void logger(RdbLogLevel l, const char *msg) {
    static char *logLevelStr[] = {
            [RDB_LOG_ERR]  = ":: ERROR ::",
            [RDB_LOG_WRN]  = ":: WARN  ::",
            [RDB_LOG_INF]  = ":: INFO  ::",
            [RDB_LOG_DBG]  = ":: DEBUG ::",
    };

    if (logfile != NULL)
        fprintf(logfile, "%s %s\n", logLevelStr[l], msg);

    if (l == RDB_LOG_ERR)
        printf("%s %s\n", logLevelStr[l], msg);
}

static void loggerWrap(RdbLogLevel l, const char *msg, ...) {
    char tmp[1024];
    va_list args;
    va_start(args, msg);
    vsnprintf(tmp, sizeof(tmp), msg, args);
    va_end(args);
    logger(l, tmp);
}

static void printUsage(int shortUsage) {
    if (shortUsage) {
        printf("Usage: rdb-cli /path/to/dump.rdb [OPTIONS] {json|resp|redis} [FORMAT_OPTIONS]\n");
        printf("For detailed usage, run command without arguments\n");
        return;
    }
    printf("[v%s] ", RDB_getLibVersion(NULL,NULL,NULL));
    printf("Usage: rdb-cli /path/to/dump.rdb [OPTIONS] {json|resp|redis} [FORMAT_OPTIONS]\n");
    printf("OPTIONS:\n");
    printf("\t-l, --log-file <PATH>         Path to the log file (Default: './rdb-cli.log')\n\n");
    printf("\tMultiple filters combination of keys/types/dbs can be specified:\n");
    printf("\t-k, --key <REGEX>             Filter keys using regex\n");
    printf("\t-K  --no-key <REGEX>          Exclude keys using regex\n");
    printf("\t-t, --type <TYPE>             Filter type {str|list|set|zset|hash|module|func}\n");
    printf("\t-T, --no-type <TYPE>          Exclude type {str|list|set|zset|hash|module|func}\n");
    printf("\t-d, --dbnum <DBNUM>           Filter DB number\n");
    printf("\t-D, --no-dbnum <DBNUM>        Exclude DB number\n\n");

    printf("FORMAT_OPTIONS ('json'):\n");
    printf("\t-i, --include <EXTRAS>        To include: {aux-val|func}\n");
    printf("\t-f, --flatten                 Print flatten json, without DBs Parenthesis\n");
    printf("\t-o, --output <FILE>           Specify the output file. If not specified, output goes to stdout\n\n");

    printf("FORMAT_OPTIONS ('resp'):\n");
    printf("\t-r, --support-restore         Use the RESTORE command when possible\n");
    printf("\t-t, --target-redis-ver <VER>  Specify the target Redis version. Helps determine which commands can\n");
    printf("\t                              be applied. Particularly crucial if support-restore being used \n");
    printf("\t                              as RESTORE is closely tied to specific RDB versions. If versions not\n");
    printf("\t                              aligned the parser will generate higher-level commands instead.\n");
    printf("\t-o, --output <FILE>           Specify the output file. If not specified, output goes to stdout\n\n");

    printf("FORMAT_OPTIONS ('redis'):\n");
    printf("\t-r, --support-restore         Use the RESTORE command when possible\n");
    printf("\t-t, --target-redis-ver <VER>  Specify the target Redis version\n");
    printf("\t-h, --hostname <HOSTNAME>     Specify the server hostname (default: 127.0.0.1)\n");
    printf("\t-p, --port <PORT>             Specify the server port (default: 6379)\n");
    printf("\t-l, --pipeline-depth <VALUE>  Number of pending commands before blocking for responses\n");
}

static RdbRes formatJson(RdbParser *parser, char *input, int argc, char **argv) {
    const char *includeArg;
    const char *output = NULL;/*default:stdout*/
    int includeFunc=0, includeAuxField=0, flatten=0; /*without*/

    /* parse specific command options */
    for (int at = 1; at < argc; ++at) {
        char *opt = argv[at];
        if (getOptArg(argc, argv, &at, "-o", "--output", opt, NULL, &output)) continue;
        if (getOptArg(argc, argv, &at, "-f", "--flatten", opt, &flatten, NULL)) continue;

        if (getOptArg(argc, argv, &at, "-i", "--include", opt, NULL, &includeArg)) {
            if (strcmp(includeArg, "aux-val") == 0) { includeAuxField = 1; continue; }
            if (strcmp(includeArg, "func") == 0) { includeFunc = 1; continue; }
            fprintf(stderr, "Invalid argument for '--include': %s\n", includeArg);
            return RDB_ERR_GENERAL;
        }

        fprintf(stderr, "Invalid JSON [FORMAT_OPTIONS] argument: %s\n", opt);
        printUsage(1);
        return RDB_ERR_GENERAL;
    }

    RdbxToJsonConf conf = {
            .level = RDB_LEVEL_DATA,
            .encoding = RDBX_CONV_JSON_ENC_PLAIN,
            .flatten = flatten,
            .includeAuxField = includeAuxField,
            .includeFunc = includeFunc,
    };

    if (RDBX_createReaderFile(parser, input) == NULL)
        return RDB_ERR_GENERAL;

    if (RDBX_createHandlersToJson(parser, output, &conf) == NULL)
        return RDB_ERR_GENERAL;

    return RDB_OK;
}

static RdbRes formatRedis(RdbParser *parser, char *input, int argc, char **argv) {
    int port = 6379;
    int pipeDepthVal=0;
    RdbxToResp *rdbToResp;
    const char *hostname = "127.0.0.1";
    const char *portStr=NULL;
    const char *pipelineDepth=NULL;

    RdbxToRespConf conf = { 0 };

    /* parse specific command options */
    for (int at = 1; at < argc; ++at) {
        char *opt = argv[at];
        if (getOptArg(argc, argv, &at, "-h", "--hostname", opt, NULL, &hostname)) continue;
        if (getOptArg(argc, argv, &at, "-p", "--port", opt, NULL, &portStr)) continue;
        if (getOptArg(argc, argv, &at, "-r", "--support-restore", opt, &(conf.supportRestore), NULL)) continue;
        if (getOptArg(argc, argv, &at, "-t", "--target-redis-ver", opt, NULL, &(conf.dstRedisVersion))) continue;
        if (getOptArg(argc, argv, &at, "-l", "--pipeline-depth", opt, NULL, &pipelineDepth)) continue;

        fprintf(stderr, "Invalid REDIS [FORMAT_OPTIONS] argument: %s\n", opt);
        printUsage(1);
        return RDB_ERR_GENERAL;
    }

    if ((pipelineDepth) && ((pipeDepthVal = atoi(pipelineDepth)) == 0)) {
        logger(RDB_LOG_ERR, "Value of '--pipeline-depth' ('-l') must be positive integer, bigger than 0");
        return RDB_ERR_GENERAL;
    }

    if (portStr) {
        port = atoi(portStr);
        if (port == 0) {
            loggerWrap(RDB_LOG_ERR, "Invalid port: %s\n", portStr);
            return RDB_ERR_GENERAL;
        }
    }

    if (RDBX_createReaderFile(parser, input) == NULL)
        return RDB_ERR_GENERAL;

    if ((rdbToResp = RDBX_createHandlersToResp(parser, &conf)) == NULL)
        return RDB_ERR_GENERAL;

    if (RDBX_createRespToRedisTcp(parser, rdbToResp, hostname, port) == NULL)
        return RDB_ERR_GENERAL;

    return RDB_OK;
}

static RdbRes formatResp(RdbParser *parser, char *input, int argc, char **argv) {
    RdbxToResp *rdbToResp;
    const char *output = NULL;/*default:stdout*/

    RdbxToRespConf conf = { 0 };

    /* parse specific command options */
    for (int at = 1; at < argc; ++at) {
        char *opt = argv[at];
        if (getOptArg(argc, argv, &at, "-o", "--output", opt, NULL, &output)) continue;
        if (getOptArg(argc, argv, &at, "-r", "--support-restore", opt, &(conf.supportRestore), NULL)) continue;
        if (getOptArg(argc, argv, &at, "-t", "--target-redis-ver", opt, NULL, &(conf.dstRedisVersion))) continue;

        fprintf(stderr, "Invalid RESP [FORMAT_OPTIONS] argument: %s\n", opt);
        printUsage(1);
        return RDB_ERR_GENERAL;
    }

    if (RDBX_createReaderFile(parser, input) == NULL)
        return RDB_ERR_GENERAL;

    if ((rdbToResp = RDBX_createHandlersToResp(parser, &conf)) == NULL)
        return RDB_ERR_GENERAL;


    if (RDBX_createRespToFileWriter(parser, rdbToResp, output) == NULL)
        return RDB_ERR_GENERAL;

    return RDB_OK;
}

int matchRdbDataType(const char *dataTypeStr) {
    if (!strcmp(dataTypeStr, "str")) return RDB_DATA_TYPE_STRING;
    if (!strcmp(dataTypeStr, "list")) return RDB_DATA_TYPE_LIST;
    if (!strcmp(dataTypeStr, "set")) return RDB_DATA_TYPE_SET;
    if (!strcmp(dataTypeStr, "zset")) return RDB_DATA_TYPE_ZSET;
    if (!strcmp(dataTypeStr, "hash")) return RDB_DATA_TYPE_HASH;
    if (!strcmp(dataTypeStr, "module")) return RDB_DATA_TYPE_MODULE;
    if (!strcmp(dataTypeStr, "stream")) return RDB_DATA_TYPE_STREAM;
    if (!strcmp(dataTypeStr, "func")) return RDB_DATA_TYPE_FUNCTION;

    fprintf(stderr, "Invalid TYPE argument (%s). Valid values: str, list, set, zset, hash, module, stream, func",
            dataTypeStr);
    exit(RDB_ERR_GENERAL);
}

int readCommonOptions(RdbParser *p, int argc, char* argv[], Options *options, int applyFilters) {
    const char *val;
    int at;

    /* default */
    options->logfilePath = LOG_FILE_PATH_DEF;
    options->formatFunc = formatJson;

    /* parse common options until FORMAT (json/resp/redis) specified */
    for (at = 2; at < argc; ++at) {
        char *opt = argv[at];

        if (getOptArg(argc, argv, &at, "-l", "--log-file", opt, NULL, &(options->logfilePath)))
            continue;

        if (getOptArg(argc, argv, &at, "-k", "--key", opt, NULL, &val)) {
            if (applyFilters) RDBX_createHandlersFilterKey(p, val, 0 /*exclude*/);
            continue;
        }

        if (getOptArg(argc, argv, &at, "-K", "--no-key", opt, NULL, &val)) {
            if (applyFilters) RDBX_createHandlersFilterKey(p, val, 1 /*exclude*/);
            continue;
        }

        if (getOptArg(argc, argv, &at, "-t", "--type", opt, NULL, &val)) {
            if (applyFilters) RDBX_createHandlersFilterType(p, matchRdbDataType(val), 0);
            continue;
        }

        if (getOptArg(argc, argv, &at, "-T", "--no-type", opt, NULL, &val)) {
            if (applyFilters) RDBX_createHandlersFilterType(p, matchRdbDataType(val), 1);
            continue;
        }

        if (getOptArg(argc, argv, &at, "-d", "--dbnum", opt, NULL, &val)) {
            if (applyFilters) RDBX_createHandlersFilterDbNum(p, atoi(val), 0);
            continue;
        }

        if (getOptArg(argc, argv, &at, "-D", "--no-dbnum", opt, NULL, &val)) {
            if (applyFilters) RDBX_createHandlersFilterDbNum(p, atoi(val), 1);
            continue;
        }

        if (strcmp(opt, "json") == 0) { options->formatFunc = formatJson; break; }
        else if (strcmp(opt, "resp") == 0) { options->formatFunc = formatResp; break; }
        else if (strcmp(opt, "redis") == 0) { options->formatFunc = formatRedis; break; }

        fprintf(stderr, "At argv[%d], unexpected OPTIONS argument: %s\n", at, opt);
        printUsage(1);
        exit(RDB_ERR_GENERAL);
    }
    return at;
}

int main(int argc, char **argv)
{
    Options options;
    RdbStatus status;
    int at;
    RdbRes res;

    if (argc < 2) {
        printUsage(0);
        return 1;
    }

    /* first argument is expected to be input file */
    char *input = argv[1];

    /* Initially, read common options that are applicable to all formatters. To
     * make it effective, apply filters later (applyFilters), ensuring that they
     * are registered only after the FORMATTER registers its own handlers. */
    at = readCommonOptions(NULL, argc, argv, &options, 0);

    if (at == argc) {
        logger(RDB_LOG_ERR, "Missing <FORMAT> value.");
        printUsage(1);
        return RDB_ERR_GENERAL;
    }

    if ((logfile = fopen(options.logfilePath, "w")) == NULL) {
        printf("Error opening log file for writing: %s \n", options.logfilePath);
        return RDB_ERR_GENERAL;
    }

    /* create the parser and attach it a file reader */
    RdbParser *parser = RDB_createParserRdb(NULL);
    RDB_setLogLevel(parser, RDB_LOG_INF);
    RDB_setLogger(parser, logger);

    if (RDB_OK != (res = options.formatFunc(parser, input, argc - at, argv + at)))
        return res;

    if (RDB_OK != RDB_getErrorCode(parser))
        return RDB_getErrorCode(parser);

    /* now that the formatter got registered, attach filters */
    readCommonOptions(parser, argc, argv, &options, 1);

    while ((status = RDB_parse(parser)) == RDB_STATUS_WAIT_MORE_DATA);

    if (status != RDB_STATUS_OK)
        return RDB_getErrorCode(parser);

    RDB_deleteParser(parser);
    fclose(logfile);
    return 0;
}
