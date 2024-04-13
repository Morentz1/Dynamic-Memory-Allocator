/**
 * Do not submit your assignment with a main function in this file.
 * If you submit with a main function in this file, you will get a zero.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "debug.h"
#include "sfmm.h"
#include <errno.h>

//get_payload_size gets payload_size
sf_size_t get_payload_size(sf_block *bp){
    bp->header ^= MAGIC;
    sf_size_t result = (bp->header & 0xFFFFFFFF00000000) >> 32;
    bp->header ^= MAGIC;
    return result;
}
//get_block_size gets block_size
sf_size_t get_block_size(sf_block *bp){
    bp->header ^= MAGIC;
    sf_size_t result =  bp->header & 0xFFFFFFF0;
    bp->header ^= MAGIC;
    return result;
}
//get_alloc_bit gets alloc bit
int get_alloc_bit(sf_block *bp){
    bp->header ^= MAGIC;
    int result = ( bp->header & THIS_BLOCK_ALLOCATED) >> 2;
    bp->header ^= MAGIC;
    return result;
}
//get_prev_alloc_bit gets prev_alloc bit
int get_palloc_bit(sf_block *bp){
    bp->header ^= MAGIC;
    int result= (bp->header & PREV_BLOCK_ALLOCATED )>> 1;
    bp->header ^= MAGIC;
    return result;
}
//get_in_quick_list_bit gets iqlist bit
int get_in_quick_list_bit(sf_block *bp){
    bp->header ^= MAGIC;
    int result = bp->header & IN_QUICK_LIST ;
    bp->header ^= MAGIC;
    return result;
}

//takes a sf_block * and builds the sf_block header with the given info and obfuscates it
void sf_make_header(sf_block *bp, sf_size_t pld_sz, sf_size_t blk_sz, int unused, int al, int pal, int iqlist ){
    //set block header to payload size
    bp->header = pld_sz;

    //shift 32 bits to the left
    bp-> header = bp->header << 32;

    //add the block size
    bp->header += blk_sz;

    //set unused bit
    if(unused == 1){
        bp->header += 8;
    }

    //set alloc bit
    if(al == 1){
        bp->header += 4;
    }

    //set prev alloc bit
    if(pal == 1){
        bp->header += 2;
    }

    //set in quick list bit
    if(iqlist == 1){
        bp->header += 1;
    }

    //obfuscate header
    bp->header ^= MAGIC;
}

sf_block *add_new_page(){
    void *new_page_start = sf_mem_grow();
    if(new_page_start == NULL){
        return NULL;
    }
    sf_block *block = (sf_block *)((uintptr_t)new_page_start-16);
    sf_header epilogue = block->header;
    sf_footer prev_footer = block->prev_footer;
    sf_make_header(block, 0, 1024, 0, 0, ((prev_footer ^ MAGIC)&0x4)>>2, 0);
    sf_block *footer = (sf_block *)((uintptr_t)block+get_block_size(block));
    footer->prev_footer = block->header;
    footer->header = epilogue;
    return block;
}

//takes a sf_block * and places it in the designated free list
//returns 0 on success, -1 on fail
int sf_put_block_in_freelist(sf_block *bp){
    //bitwise & 0xFFFFFFF0 to get blk_sz
    sf_size_t blk_sz = get_block_size(bp);

    //find the class size for the block
    int i;
    sf_size_t j;
    for(i = 0; i < NUM_FREE_LISTS; i++){
        j = (1 << i);
        j *= 32;
        //once found, insert into the front of circular doubly linked list
        if(blk_sz <= j){
            sf_block *sentinel = &sf_free_list_heads[i];
            sf_block *temp = sentinel->body.links.next;
            bp->body.links.prev = sentinel;
            bp->body.links.next = temp;
            temp->body.links.prev = bp;
            sentinel->body.links.next = bp;
            return 0;
        }
    }
    if(blk_sz > j){
        sf_block *sentinel = &sf_free_list_heads[i-1];
        sf_block *temp = sentinel->body.links.next;
        bp->body.links.prev = sentinel;
        bp->body.links.next = temp;
        temp->body.links.prev = bp;
        sentinel->body.links.next = bp;
        return 0;
    }
    return -1;
}

int sf_remove_block_in_freelist(sf_block *bp){
    sf_size_t blk_sz = get_block_size(bp);
    if(blk_sz > (1 << (NUM_FREE_LISTS-1))*32){
        sf_block *sentinel = &sf_free_list_heads[NUM_FREE_LISTS-1];
        sf_block *curr = sentinel->body.links.next;
        while(curr != sentinel){
            if(curr == bp){
                sf_block *prev = curr->body.links.prev;
                sf_block *next = curr->body.links.next;
                prev->body.links.next = next;
                next->body.links.prev = prev;
                return 0;
            }
            else{
                curr = curr->body.links.next;
            }
        }
        //block not found
        return -1;
    }
    //find the class size for the block
    int i;
    for(i = 0; i < NUM_FREE_LISTS; i++){
        int j = 1 << i;

        //once found, insert into the front of circular doubly linked list
        if(blk_sz <= j*32){
            sf_block *sentinel = &sf_free_list_heads[i];
            sf_block *first = sentinel->body.links.next;
            while(first != sentinel){
                if(first == bp){
                    //remove
                    sf_block *curr_prev = first->body.links.prev;
                    sf_block *curr_next = first->body.links.next;
                    curr_prev->body.links.next = curr_next;
                    curr_next->body.links.prev = curr_prev;
                    return 0;
                }
                else{
                    first = first->body.links.next;
                }
            }
            return -1;
        }
    }
    return -1;
}

//coalses blocks prev or next or both and adds to free_list
sf_block *coalesce(sf_block *bp){
    //case 1 b1 -> b2
    sf_size_t block_size = get_block_size(bp);
    sf_block *bp_next = (sf_block *)((uintptr_t)bp+block_size);
    int prev_alloc = get_palloc_bit(bp);
    int next_alloc =  get_alloc_bit(bp_next);

    sf_size_t coalesce_size;
    int success_code;
    //trigger case b3 <- b1 -> b2
    if( (prev_alloc == 0) && (next_alloc == 0)){
        bp->prev_footer ^= MAGIC;
        sf_size_t prev_block_size = bp->prev_footer & 0xFFFFFFF0;
        bp->prev_footer ^= MAGIC;
        sf_block *prev_block = (sf_block *)((uintptr_t)bp-prev_block_size);
        coalesce_size = (prev_block_size + get_block_size(bp_next) + block_size);

        success_code = sf_remove_block_in_freelist(bp);
        if(success_code == -1){
            return NULL;
        }
        success_code = sf_remove_block_in_freelist(bp_next);
        if(success_code == -1){
            return NULL;
        }
        success_code = sf_remove_block_in_freelist(prev_block);
        if(success_code == -1){
            return NULL;
        }
        sf_make_header(prev_block, 0, coalesce_size, 0, 0, get_palloc_bit(prev_block), 0);
        sf_block *footer = (sf_block *)((uintptr_t)prev_block+coalesce_size);
        footer->prev_footer = prev_block->header;
        return prev_block;

    } //trigger case b2 <- b1
    else if( (prev_alloc == 0) && (next_alloc == 1) ){
        bp->prev_footer ^= MAGIC;
        sf_size_t prev_block_size = bp->prev_footer & 0xFFFFFFF0;
        bp->prev_footer ^= MAGIC;
        sf_block *prev_block = (sf_block *)((char *)bp-prev_block_size);
        coalesce_size = (prev_block_size + block_size);

        success_code = sf_remove_block_in_freelist(bp);
        if(success_code == -1){
            return NULL;
        }
        success_code = sf_remove_block_in_freelist(prev_block);
        if(success_code == -1){
            return NULL;
        }
        sf_make_header(prev_block, 0, coalesce_size, 0, 0, get_palloc_bit(prev_block), 0);
        sf_block *footer = (sf_block *)((uintptr_t)prev_block+coalesce_size);
        footer->prev_footer = prev_block->header;
        sf_make_header(footer, get_payload_size(footer), get_block_size(footer), 0, get_alloc_bit(footer), 0, get_in_quick_list_bit(footer));
        return prev_block;

    }//trigger case b1 -> b2
    else if( (prev_alloc == 1) && (next_alloc == 0) ){
        coalesce_size = block_size+ get_block_size(bp_next);

        success_code = sf_remove_block_in_freelist(bp_next);
        if(success_code == -1){
            return NULL;
        }
        success_code = sf_remove_block_in_freelist(bp);
        if(success_code == -1){
            return NULL;
        }
        sf_make_header(bp, 0, coalesce_size, 0, 0, get_palloc_bit(bp), 0);
        sf_block *footer = (sf_block *)((uintptr_t)bp+coalesce_size);
        footer->prev_footer = bp->header;
        return bp;
    } //triger none (cannot be coalesce)
    else{
        success_code = sf_remove_block_in_freelist(bp);
        if(success_code == -1){
            return NULL;
        }
        return bp;
    }

    return NULL;
}


//checks if requested size block exists in free_list (best fit)
//if found, then return sf_block * of the appropriate size
//else sf_block * was not found so return NULL
sf_block *check_free_list(sf_size_t size){
    int i;
    sf_size_t j;
    for(i = 0; i < NUM_FREE_LISTS; i++){
        j = (1 << i);
        j *= 32;
        sf_block *sentinel = &sf_free_list_heads[i];
        //if block is big enough to meet request and the free list head points to a block then return that block
        //else return NULL
        if( (size <= j ) && (sentinel->body.links.next != sentinel) ){
            //class size found and now check if there exists block that satisfies the request size
            sf_block *blockp = sentinel->body.links.next;
            while(blockp != sentinel){
                sf_size_t blockp_size = get_block_size(blockp);
                if(blockp_size < size){
                    blockp = blockp->body.links.next;
                }
                else{
                    sf_block *temp_next = blockp->body.links.next;
                    sf_block *temp_prev = blockp->body.links.prev;
                    temp_prev->body.links.next = temp_next;
                    temp_next->body.links.prev = temp_prev;
                    return blockp;
                }
            }
            return NULL;
        }
    }

    return NULL;
}

//checks if a requested size block exists in quicklist
//if found, then return the sf_block * of the appropriate size
//else return null
sf_block *check_quick_list(sf_size_t size){

    //if size > max size in quick list, then return NULL
    if( size > 176){
        return NULL;
    }

    //find the index where the appropriate class is
    int index = (size-32)/16;
    if(index >= NUM_QUICK_LISTS){
        return NULL;
    }

    //if the quick_list is empty, then return NULL
    //else pop the first block and set first to first.next and decrement length
    if(sf_quick_lists[index].length == 0){
        return NULL;
    }
    else{
        sf_block *bp = sf_quick_lists[index].first;
        sf_quick_lists[index].first = bp->body.links.next;
        sf_quick_lists[index].length--;
        return bp;
    }
}

//finds block_sz for given payload
sf_size_t find_block_sz(sf_size_t payload_size){
    sf_size_t request = payload_size+8;
    sf_size_t block_sz ;
    if(request < 32){
        block_sz = 32;
    }else{
        int rem = request % 16;
        if(rem != 0){
            block_sz = request + (16-rem);
        }
        else{
            block_sz = request;
        }
    }
    return block_sz;
}


void flush_quick_list(int index){
    for(int i = 0; i < QUICK_LIST_MAX ; i++){
        sf_block *block = sf_quick_lists[index].first;
        sf_quick_lists[index].first = block->body.links.next;


        sf_make_header(block, 0, get_block_size(block), 0, 0, get_palloc_bit(block), 0 );
        sf_block *footer = (sf_block *)((uintptr_t)block+get_block_size(block));
        footer->prev_footer = block->header;


        sf_put_block_in_freelist(block);
        sf_block *coal = coalesce(block);
        sf_put_block_in_freelist(coal);
    }
}


int insert_block_quick_list(sf_block *bp, sf_size_t block_size){
    int index = (block_size-32)/16;
    if(sf_quick_lists[index].length == QUICK_LIST_MAX ){
        //flush out the quick list
        flush_quick_list(index);

        sf_size_t prev_block_size = ((bp->prev_footer ^ MAGIC) & THIS_BLOCK_ALLOCATED) >> 2;
        sf_make_header(bp, get_payload_size(bp), get_block_size(bp), 0, get_alloc_bit(bp), prev_block_size, 1 );
    }
    bp->body.links.next = sf_quick_lists[index].first;
    sf_quick_lists[index].first = bp;
    sf_quick_lists[index].length++;

    return 0;

}


//takes in sf_block *block_to_split and returns new sf_block * = sizeof(requested)
//programmer needs to check before calling if block splitting causes splinter
sf_block *split_block(sf_block *block_to_split, sf_size_t requested, sf_size_t payload_size){
    //make split half first
    sf_size_t sec_half_sz = get_block_size(block_to_split)-requested;
    sf_block *sec_half_p = (sf_block *)((uintptr_t)block_to_split+requested);
    sf_make_header(sec_half_p, 0, sec_half_sz, 0, 0, 1,0);
    sf_block *footer = (sf_block *)((uintptr_t)sec_half_p+sec_half_sz);
    footer->prev_footer = sec_half_p->header;
    sf_make_header(footer, get_payload_size(footer), get_block_size(footer), 0, get_alloc_bit(footer), 0, 0);
    sf_put_block_in_freelist(sec_half_p);
    sf_block *coal = coalesce(sec_half_p);
    int i = sf_put_block_in_freelist(coal);
    if(i == -1){
        return NULL;
    }

    //make requested block
    sf_block *request = block_to_split;
    sf_make_header(request, payload_size, requested, 0 , 1, ((block_to_split->header ^ MAGIC) & PREV_BLOCK_ALLOCATED)>>1, 0 );
    sf_block *request_footer = (sf_block *)((char *)request+requested);
    request_footer->prev_footer = request->header;
    return request;
}


//traverses through the sf_free_list_heads and sets the sentinel.next and sentinel.prev to sentinel
void initialize_freelist_heads(){
    int i;
    for(i = 0; i < NUM_FREE_LISTS; i++){
        sf_block *sp = &sf_free_list_heads[i];
        sp->body.links.next = sp;
        sp->body.links.prev = sp;
    }
}

//traverses throough sf_quick_lists and sets length = 0 and first = NULL
void initialize_quick_list(){
    int i ;
    for(i = 0; i < NUM_QUICK_LISTS; i++){
        sf_quick_lists[i].length = 0;
        sf_quick_lists[i].first = NULL;
    }
}

void *initialize_heap(){
    sf_block *pp = (sf_block *)sf_mem_grow();
    if(pp == NULL){
        return NULL;
    }

    //intialize free list heads
    initialize_freelist_heads();

    //intialize quick list
    initialize_quick_list();

    //make prologue block
    pp->prev_footer = 0;
    sf_make_header(pp, 0, 32, 0, 1, 0, 0);

    //make first free block
    sf_block *fp = (sf_block *) ((char *)pp+32);
    fp->prev_footer = pp->header;
    sf_make_header(fp, 0, 976, 0, 0, 1, 0);
    int insert_freelist_code = sf_put_block_in_freelist(fp);
    if(insert_freelist_code == -1){
        return NULL;
    }

    //make epilogue block
    sf_block *ep = (sf_block *) ((char *)fp+976);
    ep->prev_footer = fp->header;
    sf_make_header(ep, 0, 0, 0, 1, 0, 0);

    //return start of the heap
    return pp;
}

int check_valid_free_pointer(void *pp){
    if(pp == NULL){
        return -1;
    }
    if ( (uintptr_t)pp % 16 != 0 ){
        return -1;
    }

    sf_block *p = (sf_block *) ( (uintptr_t)pp-16);
    sf_size_t block_size = ((p->header ^ MAGIC) & 0xFFFFFFF0);
    if(block_size < 32 || block_size % 16 != 0){
        return -1;
    }
    void *start = sf_mem_start();
    start = (void *)( (uintptr_t)start+8);
    void *end = sf_mem_end();
    end = (void *)( (uintptr_t)end-8);
    if( ((uintptr_t)p <  (uintptr_t)start )){
        return -1;
    }
    if((uintptr_t)p >  (uintptr_t)end ){
        return -1;
    }

    int alloc_bit = ((p->header ^ MAGIC) & 0x4) >> 2;
    if(alloc_bit == 0){
        return -1;
    }

    int pal_bit =( (p->header ^ MAGIC) & 0x2) >> 1;
    int prev_block_alloc =  ((p->prev_footer ^ MAGIC) & 0x4) >> 2;
    if(pal_bit != prev_block_alloc){
        return -1;
    }

    return 0;
}

int agg_payload = 0;
int max_agg_payload = 0;

void *sf_malloc(sf_size_t size) {
    // if size is zero, then return NULL without setting errno
    if(size == 0){
        return NULL;
    }
    //check if heap is initalized
    void *heap_start = sf_mem_start();
    void *heap_end = sf_mem_end();
    if(heap_start == heap_end){
        heap_start = initialize_heap();
        if(heap_start == NULL){
            sf_errno = ENOMEM;
            return NULL;
        }
    }
    // first determine block size
    sf_size_t block_sz =find_block_sz(size);

    //check quick list for available block. If found, update header and return pointer to payload
    sf_block *bp = check_quick_list(block_sz);
    if(bp != NULL){
        //deobfuscate header
        bp->header ^= MAGIC;
        //update header
        sf_make_header(bp, size, block_sz, 0, 1, get_palloc_bit(bp) , 0);
        void *payload = (void *) &(bp->body.payload[0]);
        agg_payload += size;
        if ( agg_payload > max_agg_payload ){
            max_agg_payload = agg_payload;
        }
        return payload;
    }

    //check free list for available block. If found, update header and return pointer to payload
    bp = check_free_list(block_sz);
    if(bp != NULL){
        //get block size
        sf_size_t free_list_block_size = get_block_size(bp);

        //if splitting the block causes splinters, then return the block
        //else split the block into block_sz and (free_list_block_size - block_sz)
        if(free_list_block_size - block_sz < 32){
            sf_make_header(bp, size, free_list_block_size, 0, 1, get_palloc_bit(bp), 0 );
            sf_block *footer = (sf_block *)((uintptr_t)bp+free_list_block_size);
            footer->prev_footer = bp->header;
            void *payload = (void *)(&(bp->body.payload[0]));
            agg_payload += size;
            if ( agg_payload > max_agg_payload ){
                max_agg_payload = agg_payload;
            }
            return payload;
        }else{
            sf_block *requested_block = split_block(bp, block_sz, size);
            void *payload = (void *)(&(requested_block->body.payload[0]));
            agg_payload += size;
            if ( agg_payload > max_agg_payload ){
                max_agg_payload = agg_payload;
            }
            return payload;
        }
    }
            while(bp == NULL){
                sf_block *new_page_p = add_new_page();
                if(new_page_p == NULL){
                    sf_errno = ENOMEM;
                    return NULL;
                }
                sf_put_block_in_freelist(new_page_p);
                sf_block *coal = coalesce(new_page_p);
                int success = sf_put_block_in_freelist(coal);
                if(success == -1){
                    sf_errno = ENOMEM;
                    return NULL;
                }
                bp = check_free_list(block_sz);
            }
            sf_size_t free_list_block_size = get_block_size(bp);

            //if splitting the block causes splinters, then return the block
            //else split the block into block_sz and (free_list_block_size - block_sz)
            if(free_list_block_size - block_sz < 32){
                sf_make_header(bp, size, free_list_block_size, 0, 1, get_palloc_bit(bp), 0 );
                sf_block *footer = (sf_block *)((char *)bp+free_list_block_size);
                footer->prev_footer = bp->header;
                void *payload = (void * ) (& (bp->body.payload[0]));
                agg_payload += size;
                if ( agg_payload > max_agg_payload ){
                    max_agg_payload = agg_payload;
                }
                return payload;
            }else{
                sf_block *requested_block = split_block(bp, block_sz, size);
                void *payload = (void *) (&(requested_block->body.payload[0]));
                agg_payload += size;
                if ( agg_payload > max_agg_payload ){
                    max_agg_payload = agg_payload;
                }
                return payload;
            }
}

void sf_free(void *pp) {
    if( check_valid_free_pointer(pp) == -1){
        abort();
    }

    sf_block *bp = (sf_block *) ((uintptr_t)pp-16);
    sf_size_t blk_size = get_block_size(bp);

    if(blk_size < 176){
        agg_payload -= get_payload_size(bp);
        sf_make_header(bp, 0, blk_size, 0, 1, get_palloc_bit(bp), 1 );
        sf_block *footer = (sf_block *)((uintptr_t)bp+blk_size);
        footer->prev_footer = bp->header;
        int i = insert_block_quick_list(bp, blk_size);
        if(i ==-1){
            abort();
        }
    }
    else{
        agg_payload -= get_payload_size(bp);
        sf_make_header(bp, 0, get_block_size(bp), 0, 0, get_palloc_bit(bp), 0 );
        sf_block *footer = (sf_block *)((uintptr_t)bp+blk_size);
        footer->prev_footer = bp->header;
        sf_make_header(footer, get_payload_size(footer), get_block_size(footer), 0, get_alloc_bit(footer), 0, get_palloc_bit(footer));
        sf_put_block_in_freelist(bp);
        sf_block *coal = coalesce(bp);
        //sf_put_block_in_freelist(coal);
        sf_put_block_in_freelist(coal);
    }
}

void *sf_realloc(void *pp, sf_size_t rsize) {
    if(check_valid_free_pointer(pp) == -1){
        sf_errno = EINVAL;
        return NULL;
    }
    if(rsize == 0){
        sf_free(pp);
        return NULL;
    }

    sf_block *bp = (sf_block *) ((uintptr_t)pp-16);
    sf_size_t blk_size = get_block_size(bp);
    sf_size_t block_resize = find_block_sz(rsize);
    if(block_resize > blk_size){
        void *larger_payload = sf_malloc(rsize);
        memcpy(larger_payload, pp, get_payload_size(bp) );
        sf_free(pp);
        return larger_payload;
    }
    if(block_resize < blk_size){
        if(blk_size - block_resize < 32){
            sf_make_header(bp, rsize, blk_size, 0, 1, get_palloc_bit(bp), 0 );
            sf_block *footer = (sf_block *)((char *)bp+blk_size);
            footer->prev_footer = bp->header;
            void *payload = (void *)(&(bp->body.payload[0]));
            return payload;
        }else{
            sf_block *requested_block = split_block(bp, block_resize, rsize);
            void *payload = (void *)(&(requested_block->body.payload[0]));
            return payload;
        }
    }
    return NULL;
}

double sf_internal_fragmentation() {
    // TO BE IMPLEMENTED
    void *start = sf_mem_start();
    void *end = sf_mem_end();
    if(start == end){
        return 0.0;
    }
    sf_size_t total_payload_size = 0;
    sf_size_t total_block_size = 0;
    sf_block *prologue = (sf_block *)start;
    sf_block *curr = (sf_block *)((uintptr_t)prologue+get_block_size(prologue));
    sf_block *epilogue = (sf_block *)((uintptr_t)end-16);
    while(curr != epilogue){
        if(get_alloc_bit(curr) == 1){
            total_payload_size += (double) (get_payload_size(curr));
            total_block_size += (double) (get_block_size(curr));
        }
        curr = (sf_block*)((uintptr_t)curr+get_block_size(curr));
    }
    double frag = 0.0;
    if(total_block_size == 0.0){
        return frag;
    }
    frag = ((double)total_payload_size/(double)total_block_size);
    return frag;
}


double sf_peak_utilization() {
    double util = 0.0;
    if(sf_mem_start() == sf_mem_end()){
        return util;
    }
    else{
        double heap_size = (double)((uintptr_t)sf_mem_end() - (uintptr_t)sf_mem_start());
        util = ((double)max_agg_payload)/heap_size;
        return util;
    }
    // TO BE IMPLEMENTED
    abort();
}
