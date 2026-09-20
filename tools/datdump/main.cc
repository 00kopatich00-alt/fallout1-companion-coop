// Standalone dev tool: inspect the contents of a Fallout 1 DAT archive
// (MASTER.DAT / CRITTER.DAT) using the game's own archive-reading code.
//
// Usage:
//   datdump <file.dat> dirs
//   datdump <file.dat> list <dirname>
//   datdump <file.dat> extract <dirname>\<filename> <outpath>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "plib/db/db.h"

using namespace fallout;

int main(int argc, char** argv)
{
    if (argc < 3) {
        printf("Usage:\n");
        printf("  datdump <file.dat> dirs\n");
        printf("  datdump <file.dat> list <dirname>\n");
        printf("  datdump <file.dat> extract <dirname>\\<filename> <outpath>\n");
        return 1;
    }

    const char* datfile = argv[1];
    const char* cmd = argv[2];

    DB_DATABASE* db = db_init(datfile, "", "", 0);
    if (db == INVALID_DATABASE_HANDLE) {
        printf("Failed to open %s\n", datfile);
        return 1;
    }
    db_select(db);

    if (strcmp(cmd, "dirs") == 0) {
        char** dirs;
        int n = db_dump_list_dirs(&dirs);
        for (int i = 0; i < n; i++) {
            printf("%s\n", dirs[i]);
        }
        db_dump_free_list(dirs, n);
    } else if (strcmp(cmd, "list") == 0 && argc >= 4) {
        char** files;
        dir_entry* entries;
        int n = db_dump_list_files(argv[3], &files, &entries);
        for (int i = 0; i < n; i++) {
            printf("%s\t%d\t%d\n", files[i], entries[i].length, entries[i].flags);
        }
        db_dump_free_list(files, n);
        free(entries);
    } else if (strcmp(cmd, "extract") == 0 && argc >= 5) {
        DB_FILE* f = db_fopen(argv[3], "rb");
        if (f == NULL) {
            printf("Not found: %s\n", argv[3]);
            db_close(db);
            return 1;
        }

        long len = db_filelength(f);
        unsigned char* buf = (unsigned char*)malloc(len);
        db_fread(buf, 1, len, f);
        db_fclose(f);

        FILE* out = fopen(argv[4], "wb");
        if (out == NULL) {
            printf("Cannot write %s\n", argv[4]);
            free(buf);
            db_close(db);
            return 1;
        }
        fwrite(buf, 1, len, out);
        fclose(out);
        free(buf);

        printf("Extracted %ld bytes to %s\n", len, argv[4]);
    } else {
        printf("Unknown command or missing arguments.\n");
    }

    db_close(db);
    return 0;
}
