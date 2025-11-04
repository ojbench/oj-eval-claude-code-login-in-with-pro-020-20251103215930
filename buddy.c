#include "buddy.h"
#define NULL ((void *)0)

#define MAXRANK 16
#define PAGE_SIZE 4096
#define ALLOCATED_BIT 0x80  // High bit indicates allocated

// Free list for each rank - doubly linked for O(1) removal
typedef struct free_block {
    struct free_block *next;
    struct free_block *prev;
} free_block_t;

static free_block_t *free_lists[MAXRANK + 1];
static void *memory_start;
static int total_pages;
static unsigned char page_rank[65536]; // Track rank of each page
                                       // bit 7: 1=allocated, 0=free
                                       // bits 0-6: rank (1-16)

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

// Helper function to check if a block is in free list
static int is_block_in_free_list(int page_idx, int rank) {
    void *block_addr = memory_start + page_idx * PAGE_SIZE;
    free_block_t *current = free_lists[rank];
    while (current != NULL) {
        if ((void *)current == block_addr) {
            return 1;
        }
        current = current->next;
    }
    return 0;
}

// Helper function to remove a block from free list
static void remove_from_free_list(int page_idx, int rank) {
    void *block_addr = memory_start + page_idx * PAGE_SIZE;
    free_block_t *block = (free_block_t *)block_addr;

    // Remove from doubly-linked list
    if (block->prev != NULL) {
        block->prev->next = block->next;
    } else {
        // This is the head of the list
        free_lists[rank] = block->next;
    }

    if (block->next != NULL) {
        block->next->prev = block->prev;
    }
}

// Helper function to add block to free list
static void add_to_free_list(int page_idx, int rank) {
    void *block_addr = memory_start + page_idx * PAGE_SIZE;
    free_block_t *block = (free_block_t *)block_addr;

    // Add to head of doubly-linked list
    block->next = free_lists[rank];
    block->prev = NULL;

    if (free_lists[rank] != NULL) {
        free_lists[rank]->prev = block;
    }
    free_lists[rank] = block;

    // Only mark the first page of the free block with the rank
    // This is sufficient for buddy merging check
    page_rank[page_idx] = rank;  // Free block
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
        page_rank[page_idx + i] = rank | ALLOCATED_BIT;  // Allocated block
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
    int rank_byte = page_rank[page_idx];
    if ((rank_byte & ALLOCATED_BIT) == 0) {
        return -EINVAL;  // Not allocated
    }
    int rank = rank_byte & 0x7F;
    if (rank == 0 || rank > MAXRANK) {
        return -EINVAL;
    }

    // Try to merge with buddy
    while (rank < MAXRANK) {
        int pages_in_block = 1 << (rank - 1);
        int buddy_idx = get_buddy_index(page_idx, rank);

        // Check if buddy exists and is in range
        if (buddy_idx < 0 || buddy_idx + pages_in_block > total_pages) {
            break;
        }

        // Quick check: buddy should be free with the same rank
        // Free blocks have rank without ALLOCATED_BIT
        if (page_rank[buddy_idx] != rank) {
            break;  // Buddy is not free with the same rank
        }

        // Remove buddy from free list
        remove_from_free_list(buddy_idx, rank);

        // Merge with buddy
        if (buddy_idx < page_idx) {
            page_idx = buddy_idx;
        }
        rank++;
    }

    // Add merged block to free list (marks pages as free)
    add_to_free_list(page_idx, rank);

    return OK;
}

int query_ranks(void *p) {
    int page_idx = get_page_index(p);
    if (page_idx < 0) {
        return -EINVAL;
    }

    // Return the stored rank (mask out the allocated bit)
    int rank_byte = page_rank[page_idx];
    int rank = rank_byte & 0x7F;
    if (rank > 0 && rank <= MAXRANK) {
        return rank;
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
