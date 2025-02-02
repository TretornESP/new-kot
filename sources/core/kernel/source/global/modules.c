#include <errno.h>
#include <stddef.h>
#include <lib/log.h>
#include <global/vfs.h>
#include <lib/string.h>
#include <impl/panic.h>
#include <global/heap.h>
#include <global/file.h>
#include <global/modules.h>
#include <global/elf_loader.h>

volatile vfs_handler_t* vfs_handler = NULL;
volatile pci_handler_t* pci_handler = NULL;
volatile time_handler_t* time_handler = NULL;
volatile storage_handler_t* storage_handler = NULL;

static module_flags_t modules_flags[MODULE_TYPE_COUNT];

static const char* modules_cfg_path = "/initrd/system/starter/modules.cfg";

void modules_init(void){
    int err = 0;
    kernel_file_t* file = f_open(KERNEL_VFS_CTX, modules_cfg_path, 0, 0, &err);
    if(file != NULL){
        void* buffer = malloc(file->file_size_initial);
        size_t bytes_read;
        f_read(buffer, file->file_size_initial, &bytes_read, file);
        char* line = (char*)buffer;
        while(line != NULL){
            char* data = strchr(line, '=') + sizeof(char);
            char* end_of_line = strchr(line, '\n');
            char* current_line = line;
            if(end_of_line){
                *end_of_line = '\0';
                line = end_of_line + sizeof(char);
            }else{
                line = NULL;
            }

            if(strstr(current_line, "MODULE_PATH=")){
                char* args[2] = {data, NULL};
                module_metadata_t* module_metadata = NULL;
                if(!load_elf_module(&module_metadata, 1, args)){
                    modules_set_load_state(module_metadata->type, true);
                }else if(module_metadata != NULL){
                    modules_set_load_state(module_metadata->type, false);
                }
            }

        }
        free(buffer);
        f_close(file);
    }else{
        panic("file: %s not found !", modules_cfg_path);
    }
}

int modules_request_dependency(module_type_t type){
    if(type >= MODULE_TYPE_COUNT){
        return EINVAL;
    }
    while(!(modules_flags[type] & MODULE_FLAGS_LOADED)){
        // TODO : Wait
        panic("modules_request_dependency : module type %d isn't loaded yet !", type);
    }
    return 0;
}

int modules_set_load_state(module_type_t type, bool value){
    if(type >= MODULE_TYPE_COUNT){
        return EINVAL;
    }

    if(value){
        modules_flags[type] |= MODULE_FLAGS_LOADED;
    }else{
        modules_flags[type] &= ~MODULE_FLAGS_LOADED;
    }

    return 0;
}

