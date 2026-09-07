/* Headless preset creation and inspection; never loads a game or SDL. */
#include "settings.h"
#include <errno.h>
#include <string.h>
#include <sys/stat.h>

int main(int argc, char **argv) {
    const char *path=NULL, *preset=NULL, *create=NULL;
    int list=0;
    for (int i=1;i<argc;i++) {
        if (!strcmp(argv[i],"--config") || !strcmp(argv[i],"--preset") || !strcmp(argv[i],"--create-defaults")) {
            const char *key=argv[i];
            if (++i==argc) { fprintf(stderr,"%s needs a value\n",key); return 2; }
            if (!strcmp(key,"--config")) path=argv[i];
            else if (!strcmp(key,"--preset")) preset=argv[i];
            else create=argv[i];
        } else if (!strcmp(argv[i],"--list-presets")) list=1;
        else if (!strcmp(argv[i],"--print-settings")) { }
        else if (!strcmp(argv[i],"--help")) {
            puts("settings-tool [--config FILE] [--preset NAME] [--print-settings | --list-presets]\n"
                 "settings-tool --create-defaults FILE\nNo config file is read unless --config is supplied."); return 0;
        } else { fprintf(stderr,"unknown option: %s\n",argv[i]); return 2; }
    }
    char error[LR_ERROR_SIZE];
    if (create) {
        /* An explicit create must not overwrite an existing preferences file. */
        struct stat st;
        if (!lstat(create,&st)) { fprintf(stderr,"already exists: %s\n",create); return 2; }
        if (errno!=ENOENT) { fprintf(stderr,"%s: %s\n",create,strerror(errno)); return 2; }
        lr_presets p; lr_presets_defaults(&p);
        if (lr_presets_save(&p,create,error)) goto failed;
        printf("Created %s\n",create); return 0;
    }
    if (list) {
        if (!path) { fprintf(stderr,"--list-presets requires --config\n"); return 2; }
        lr_presets p;
        if (lr_presets_load(&p,path,error)) goto failed;
        for (int i=0;i<p.count;i++) printf("%c %s\n",i==p.selected?'*':' ',p.presets[i].name);
        return 0;
    }
    lr_settings s;
    if (lr_settings_load(&s,path,preset,error)) goto failed;
    lr_settings_print(&s,stdout); return 0;
failed:
    fprintf(stderr,"settings: %s\n",error); return 2;
}
