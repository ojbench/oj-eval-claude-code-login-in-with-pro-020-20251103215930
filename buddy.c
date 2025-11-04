#include "buddy.h"
#define NULL ((void *)0)

#define MAXRANK 16
#define PAGE_SIZE 4096

// Free list for each rank
typedef struct free_block {
    struct free_block *next;
} free_block_t;

static free_block_t *free_lists[MAXRANK + 1];
static void *memory_start;
static int total_pages;
static unsigned char page_rank[65536]; // Track rank of each page (0 if free, >0 if allocated)

// Helper function to get page index
static int get_page_index(void *p) {
    if (p < memory_start || p >= memory_start + total_pages * PAGE_SIZE) {
        return -1;
    }
    long offset = (char *)p - (char *)memory_start;
    if (offset % PAGE_SIZE != 0) {
        return -1;
    }
    return offset / PAGE_SIZE;
}

// Helper function to get buddy index
static int get_buddy_index(int page_idx, int rank) {
    int pages_in_block = 1 << (rank - 1);
    return page_idx ^ pages_in_block;
}

// Helper function to check if a block is free
static int is_block_free(int page_idx, int rank) {
    int pages_in_block = 1 << (rank - 1);
    for (int i = 0; i < pages_in_block; i++) {
        if (page_rank[page_idx + i] != 0) {
            return 0;
        }
    }
    return 1;
}

// Helper function to remove a block from free list
static void remove_from_free_list(int page_idx, int rank) {
    void *block_addr = memory_start + page_idx * PAGE_SIZE;
    free_block_t **current = &free_lists[rank];

    while (*current != NULL) {
        if ((void *)(*current) == block_addr) {
            *current = (*current)->next;
            return;
        }
        current = &((*current)->next);
    }
}

// Helper function to add block to free list
static void add_to_free_list(int page_idx, int rank) {
    void *block_addr = memory_start + page_idx * PAGE_SIZE;
    free_block_t *block = (free_block_t *)block_addr;
    block->next = free_lists[rank];
    free_lists[rank] = block;
}

int init_page(void *p, int pgcount) {
    memory_start = p;
    total_pages = pgcount;

    // Initialize page_rank array
    for (int i = 0; i < pgcount; i++) {
        page_rank[i] = 0;
    }

    // Initialize free lists
    for (int i = 0; i <= MAXRANK; i++) {
        free_lists[i] = NULL;
    }

    // Find the maximum rank that fits in pgcount
    int current_page = 0;
    while (current_page < pgcount) {
        int best_rank = 0;
        // Find largest rank that fits
        for (int r = MAXRANK; r >= 1; r--) {
            int pages_needed = 1 << (r - 1);
            // Check if block is aligned and fits
            if ((current_page % pages_needed == 0) && (current_page + pages_needed <= pgcount)) {
                best_rank = r;
                break;
            }
        }

        if (best_rank > 0) {
            add_to_free_list(current_page, best_rank);
            current_page += (1 << (best_rank - 1));
        } else {
            current_page++;
        }
    }

    return OK;
}

void *alloc_pages(int rank) {
    if (rank < 1 || rank > MAXRANK) {
        return ERR_PTR(-EINVAL);
    }

    // Find available block of requested rank or larger
    int found_rank = -1;
    for (int r = rank; r <= MAXRANK; r++) {
        if (free_lists[r] != NULL) {
            found_rank = r;
            break;
        }
    }

    if (found_rank == -1) {
        return ERR_PTR(-ENOSPC);
    }

    // Remove block from free list
    free_block_t *block = free_lists[found_rank];
    free_lists[found_rank] = block->next;

    int page_idx = ((char *)block - (char *)memory_start) / PAGE_SIZE;

    // Split blocks down to requested rank
    while (found_rank > rank) {
        found_rank--;
        int buddy_idx = page_idx + (1 << (found_rank - 1));
        add_to_free_list(buddy_idx, found_rank);
    }

    // Mark pages as allocated
    int pages_in_block = 1 << (rank - 1);
    for (int i = 0; i < pages_in_block; i++) {
        page_rank[page_idx + i] = rank;
    }

    return (void *)block;
}

int return_pages(void *p) {
    if (p == NULL) {
        return -EINVAL;
    }

    int page_idx = get_page_index(p);
    if (page_idx < 0) {
        return -EINVAL;
    }

    // Get the rank of this allocation
    int rank = page_rank[page_idx];
    if (rank == 0 || rank > MAXRANK) {
        return -EINVAL;
    }

    // Mark pages as free
    int pages_in_block = 1 << (rank - 1);
    for (int i = 0; i < pages_in_block; i++) {
        page_rank[page_idx + i] = 0;
    }

    // Try to merge with buddy
    while (rank < MAXRANK) {
        int pages_in_block = 1 << (rank - 1);
        int buddy_idx = get_buddy_index(page_idx, rank);

        // Check if buddy exists and is free
        if (buddy_idx < 0 || buddy_idx + pages_in_block > total_pages) {
            break;
        }

        if (!is_block_free(buddy_idx, rank)) {
            break;
        }

        // Check if buddy is actually in the free list
        int buddy_in_list = 0;
        free_block_t *current = free_lists[rank];
        void *buddy_addr = memory_start + buddy_idx * PAGE_SIZE;
        while (current != NULL) {
            if ((void *)current == buddy_addr) {
                buddy_in_list = 1;
                break;
            }
            current = current->next;
        }

        if (!buddy_in_list) {
            break;
        }

        // Remove buddy from free list
        remove_from_free_list(buddy_idx, rank);

        // Merge with buddy
        if (buddy_idx < page_idx) {
            page_idx = buddy_idx;
        }
        rank++;
    }

    // Add merged block to free list
    add_to_free_list(page_idx, rank);

    return OK;
}

int query_ranks(void *p) {
    int page_idx = get_page_index(p);
    if (page_idx < 0) {
        return -EINVAL;
    }

    // If allocated, return the allocation rank
    if (page_rank[page_idx] > 0) {
        return page_rank[page_idx];
    }

    // If free, find the maximum rank of free block containing this page
    for (int r = MAXRANK; r >= 1; r--) {
        free_block_t *current = free_lists[r];
        while (current != NULL) {
            int block_page_idx = ((char *)current - (char *)memory_start) / PAGE_SIZE;
            int pages_in_block = 1 << (r - 1);
            if (page_idx >= block_page_idx && page_idx < block_page_idx + pages_in_block) {
                return r;
            }
            current = current->next;
        }
    }

    return -EINVAL;
}

int query_page_counts(int rank) {
    if (rank < 1 || rank > MAXRANK) {
        return -EINVAL;
    }

    int count = 0;
    free_block_t *current = free_lists[rank];
    while (current != NULL) {
        count++;
        current = current->next;
    }

    return count;
}
