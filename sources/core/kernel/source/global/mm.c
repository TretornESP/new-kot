#include <lib/lock.h>
#include <lib/math.h>
#include <impl/vmm.h>
#include <global/mm.h>
#include <global/pmm.h>
#include <lib/assert.h>
#include <lib/memory.h>
#include <global/heap.h>

static int remove_region(memory_handler_t* handler, memory_region_t* region){
    if(region->last){
        region->last->next = region->next;
    }
    if(region->next){
        region->next->last = region->last;
    }
    if(handler->last_free_region == region){
        if(region->last){
            handler->last_free_region = region->last;
        }else{
            handler->last_free_region = region->next;
        }
    }
    handler->region_count--;
    memset(region, 0xff, sizeof(memory_region_t));
    free(region);
    return 0;
}

static memory_region_t* split_region(memory_handler_t* handler, memory_region_t* region, size_t size){
    if(size == 0 || region->size == size){
        return region;
    }else{
        if(region->size < size){
            return region;
        }

        memory_region_t* new_region = (memory_region_t*)malloc(sizeof(memory_region_t));
        new_region->base = (void*)((uintptr_t)region->base + size);
        new_region->size = region->size - size;
        new_region->block_count = DIV_ROUNDUP(new_region->size, PAGE_SIZE);
        new_region->is_free = region->is_free;
        new_region->last = region;
        new_region->next = region->next;

        if(region->next){
            region->next->last = new_region;
        }
        
        region->next = new_region;
        region->size = size;
        region->block_count = DIV_ROUNDUP(region->size, PAGE_SIZE);

        handler->region_count++;

        return new_region;
    }
}

static memory_region_t* merge_region(memory_handler_t* handler, memory_region_t* region){
    if(region->next){
        memory_region_t* from = region->next;
        region->size += from->size;
        region->block_count += from->block_count;
        remove_region(handler, from);
        return region;
    }else{
        return NULL;
    }
}

static memory_flags_t mm_getmemory_flags_from_prot(int prot){
    return ((prot & PROT_READ) ? MEMORY_FLAG_READABLE : 0) | ((prot & PROT_WRITE) ? MEMORY_FLAG_WRITABLE : 0) | ((prot & PROT_EXEC) ? MEMORY_FLAG_EXECUTABLE : 0);
}

memory_handler_t* mm_create_handler(vmm_space_t vmm_space, void* base, size_t size){
    memory_handler_t* handler = (memory_handler_t*)malloc(sizeof(memory_handler_t));
    handler->base = base;
    handler->size = size;
    handler->vmm_space = vmm_space;
    handler->region_count = 0;

    memory_region_t* region = (memory_region_t*)malloc(sizeof(memory_region_t));
    region->base = base;
    region->size = size;
    region->block_count = DIV_ROUNDUP(size, PAGE_SIZE);
    region->is_free = true;
    region->last = NULL;
    region->next = NULL;

    handler->first_region = region;
    handler->last_free_region = region;

    handler->region_count++;

    handler->lock = (spinlock_t){};
    return handler;
}

memory_handler_t* mm_clone_handler(vmm_space_t vmm_space, memory_handler_t* source){
    memory_handler_t* destination = (memory_handler_t*)malloc(sizeof(memory_handler_t));
    destination->base = source->base;
    destination->size = source->size;
    destination->vmm_space = vmm_space;
    destination->region_count = source->region_count;

    memory_region_t* last_region = NULL;
    memory_region_t* region = source->first_region;
    for(uint64_t i = 0; i < source->region_count; i++){
        memory_region_t* region_copy = (memory_region_t*)malloc(sizeof(memory_region_t));

        memcpy(region_copy, region, sizeof(memory_region_t));

        if(last_region){
            region_copy->last = last_region;
            last_region->next = region_copy;
        }

        if(region == source->first_region){
            destination->first_region = region_copy;
        }

        if(region == source->last_free_region){
            destination->last_free_region = region_copy;
        }

        last_region = region_copy;

        region = region->next;
    } 

    destination->lock = (spinlock_t){};

    return destination;    
}

int free_handler(memory_handler_t* handler){
    memory_region_t* region = handler->first_region;
    for(uint64_t i = 0; i < handler->region_count; i++){
        memory_region_t* next = region->next;
        free(region);
        region = next;
    }
    free(handler);
    return 0;
}


int mm_allocate_region_vm(memory_handler_t* handler, void* base, size_t size, bool is_fixed, void** base_result){
    assert(!((uintptr_t)base % PAGE_SIZE));
    assert(!(size % PAGE_SIZE));

    spinlock_acquire(&handler->lock);

    memory_region_t* region;

    if(is_fixed){
        region = mm_get_region(handler, base);
        if(!region){
            spinlock_release(&handler->lock);
            if((uintptr_t)base + (uintptr_t)size > (uintptr_t)handler->base + (uintptr_t)handler->size){
                return EINVAL;
            }else{
                *base_result = base;
                return 0;
            }
        }
        size_t size_unusable = (uintptr_t)base - (uintptr_t)region->base;
        size_t size_need = size_unusable + size;
        while(region->size < size_need){
            if(!merge_region(handler, region)){
                if((uintptr_t)region->base + (uintptr_t)region->size > (uintptr_t)handler->base + (uintptr_t)handler->size){
                    return EINVAL;
                }else{
                    break;
                }
            }
        }
        if(region->is_free){
            region = split_region(handler, region, size_unusable);
            split_region(handler, region, size);
        }
        *base_result = base; 
    }else{
        memory_region_t* region_to_split = NULL; 
        memory_region_t* tmp = handler->last_free_region;
        for(uint64_t i = 0; i < handler->region_count; i++){
            if(tmp->is_free){
                if(tmp->size >= size){
                    region_to_split = tmp;
                    break;
                }
            }
            tmp = tmp->next;
        }
        if(!region_to_split){
            spinlock_release(&handler->lock);
            return ENOMEM;
        }
        split_region(handler, region_to_split, size);
        region = region_to_split;

        *base_result = region->base;
    }

    if(region->next){
        if(!region->next->is_free){
            if(!merge_region(handler, region)){
                assert(!"not allowed !!");
            }
        }
    }
    
    region->is_free = false;

    if(region->last){
        if(!region->last->is_free){
            if(!merge_region(handler, region->last)){
                assert(!"not allowed !!");
            }
        }
    }

    spinlock_release(&handler->lock);

    return 0;
}

int mm_free_region(memory_handler_t* handler, void* base, size_t size){
    assert(!((uintptr_t)base % PAGE_SIZE));
    assert(!(size % PAGE_SIZE));

    spinlock_acquire(&handler->lock);

    memory_region_t* region = mm_get_region(handler, base);

    if(region->base != base){
        size_t size_unusable = (uintptr_t)base - (uintptr_t)region->base;
        region = split_region(handler, region, size_unusable);
    }

    if(region->size != size){
        while(region->size < size){
            if(!merge_region(handler, region)){
                return EINVAL;
            }
        }
        if(region->size > size){
            region = split_region(handler, region, size);
        }
    }

    if(region->next){
        if(region->next->is_free){
            if(!merge_region(handler, region)){
                assert(!"not allowed !!");
            }
        }
    }
    
    region->is_free = true;

    if(region->last){
        if(region->last->is_free){
            if(!merge_region(handler, region->last)){
                assert(!"not allowed !!");
            }
        }
    }

    spinlock_release(&handler->lock);

    return 0;
}

memory_region_t* mm_get_region(memory_handler_t* handler, void* base){
    if((uintptr_t)base > (uintptr_t)handler->base + (uintptr_t)handler->size){
        return NULL;
    }

    memory_region_t* region = handler->first_region;
    for(uint64_t i = 0; i < handler->region_count; i++){
        if((uintptr_t)region->base <= (uintptr_t)base && (uintptr_t)region->base + region->size > (uintptr_t)base){
            return region;
        }
        region = region->next;
    }
    return NULL;
}


int mm_allocate_memory_block(memory_handler_t* handler, void* base, size_t size, int prot, size_t* size_allocate){
    return vmm_map_allocate(handler->vmm_space, (memory_range_t){base, size}, mm_getmemory_flags_from_prot(prot), size_allocate);
}

int mm_allocate_memory_contigous(memory_handler_t* handler, void* base, size_t size, int prot, size_t* size_allocate){
    size_t page_count = DIV_ROUNDUP(size, PAGE_SIZE);
    void* base_physical = pmm_allocate_pages(page_count);

    if(base_physical == NULL){
        *size_allocate = 0;
        return ENOMEM;
    }

    *size_allocate = size;

    return mm_map_physical(handler, base_physical, base, size, prot);
}

int mm_map_physical(memory_handler_t* handler, void* base_physical, void* base, size_t size, int prot){
    return vmm_map(handler->vmm_space, (memory_range_t){base, size}, (memory_range_t){base_physical, size}, mm_getmemory_flags_from_prot(prot));
}

int mm_unmap(memory_handler_t* handler, void* base, size_t size){
    if((uintptr_t)base < (uintptr_t)handler->base){
        return EINVAL;
    }

    if((uintptr_t)base + (uintptr_t)size > (uintptr_t)handler->base + (uintptr_t)handler->size){
        return EINVAL;
    }

    return vmm_unmap(handler->vmm_space, (memory_range_t){base, size});
}

int mm_protect(memory_handler_t* handler, void* base, size_t size, int prot){
    return vmm_update_flags(handler->vmm_space, (memory_range_t){base, size}, mm_getmemory_flags_from_prot(prot));
}