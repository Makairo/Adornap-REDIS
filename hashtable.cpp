#include <assert.h>
#include <stdlib.h>     
#include "hashtable.h"

// constant work
const size_t k_rehashing_work = 128;
const size_t k_max_load_factor = 8;

// IN : HTab *htab, size_t n
// OUT : htab is initialized
// DESC: Initialize a hash table with n slots (must be a power of 2)
static void h_init(HTab *htab, size_t n)
{
    assert(n > 0 && ((n-1) & n) == 0);

    htab->tab = (HNode **)calloc(n, sizeof(HNode *));
    htab->mask = n - 1;
    htab->size = 0;
}

// IN : HTab *htab, HNode *node
// OUT : node inserted into hash table
// DESC: Insert a node into the hash table (no duplicate checking)
static void h_insert(HTab *htab, HNode *node)
{
    size_t pos = node->hcode & htab->mask;
    HNode *next = htab->tab[pos];
    node->next = next;
    htab->tab[pos] = node;
    htab->size++;
}

// IN : HTab *htab, HNode *key, bool (*eq)(HNode *, HNode *)
// OUT : returns pointer to pointer of the node if found, NULL if not
// DESC: Lookup a key in the hash table; returns the address of the pointer that owns the node
static HNode **h_lookup(HTab *htab, HNode *key, bool (*eq)(HNode *, HNode *))
{
    if(!htab->tab) return NULL;

    size_t pos = key->hcode & htab->mask;
    HNode **from = &htab->tab[pos];
    for(HNode *cur ; (cur = *from) != NULL ; from =&cur->next)
    {
        if(cur->hcode == key->hcode && eq(cur, key)) return from;
    }
    return NULL;
}

// IN : HTab *htab, HNode **from
// OUT : returns the detached node, htab updated
// DESC: Remove a node from the hash table, updating pointers; used by delete
static HNode *h_detach(HTab *htab, HNode **from)
{
    HNode *node = *from;
    *from = node->next;
    htab->size--;
    return node;
}

// IN : HMap *hmap
// OUT : migrates some nodes from oldMap to newMap
// DESC: Helper function for incremental rehashing; moves up to k_rehashing_work nodes
static void hm_help_rehashing(HMap *hmap)
{
    size_t nwork = 0;
    while(nwork < k_rehashing_work && hmap->oldMap.size > 0)
    {
        HNode **from = &hmap->oldMap.tab[hmap->migrate_pos];
        if(!*from)
        {
            hmap->migrate_pos++;
            continue;
        }
        h_insert(&hmap->newMap, h_detach(&hmap->oldMap, from));
        nwork++;
    }

    if(hmap->oldMap.size == 0 && hmap->oldMap.tab)
    {
        free(hmap->oldMap.tab);
        hmap->oldMap = HTab {};
    }
}

// IN : HMap *hmap
// OUT : oldMap and newMap updated
// DESC: Trigger full rehashing by promoting newMap to oldMap and allocating a larger newMap
static void hm_trigger_rehashing(HMap *hmap)
{
    assert(hmap->oldMap.tab == NULL);

    hmap->oldMap = hmap->newMap;
    h_init(&hmap->newMap, (hmap->newMap.mask + 1) * 2);
    hmap->migrate_pos = 0;
}

// IN : HMap *hmap, HNode *key, bool (*eq)(HNode *, HNode *)
// OUT : returns pointer to node if found, NULL if not
// DESC: Lookup a node in the hash map, performing incremental rehashing if needed
HNode *hm_lookup(HMap *hmap, HNode *key, bool (*eq)(HNode *, HNode *))
{
    hm_help_rehashing(hmap);

    HNode **from = h_lookup(&hmap->newMap, key, eq);
    if(!from) from = h_lookup(&hmap->oldMap, key, eq);

    return from ? *from : NULL;
}

// IN : HMap *hmap, HNode *node
// OUT : inserts node into newMap
// DESC: Insert a node into the hash map, triggering rehashing if load factor exceeded
void hm_insert(HMap *hmap, HNode *node)
{
    if(!hmap->newMap.tab) h_init(&hmap->newMap, 4); 

    h_insert(&hmap->newMap, node);

    if(!hmap->oldMap.tab)
    {
        size_t shreshold = (hmap->newMap.mask + 1) * k_max_load_factor;
        if(hmap->newMap.size >= shreshold) hm_trigger_rehashing(hmap);
    }

    hm_help_rehashing(hmap);
}

// IN : HMap *hmap, HNode *key, bool (*eq)(HNode *, HNode *)
// OUT : removes the node from the hash map, returns it if found, NULL if not
// DESC: Delete a key from the hash map, handling incremental rehashing
HNode *hm_delete(HMap *hmap, HNode *key, bool (*eq)(HNode *, HNode *)) 
{
    hm_help_rehashing(hmap);

    if(HNode **from = h_lookup(&hmap->newMap, key, eq)) return h_detach(&hmap->newMap, from);
    if(HNode **from = h_lookup(&hmap->oldMap, key, eq)) return h_detach(&hmap->oldMap, from);

    return NULL;
}

// IN : HMap *hmap
// OUT : frees all memory and resets map
// DESC: Clear all entries in the hash map
void hm_clear(HMap *hmap)
{
    free(hmap->newMap.tab);
    free(hmap->oldMap.tab);
    *hmap = HMap {};
}

// IN : HMap *hmap
// OUT : returns the total number of nodes in the hash map
// DESC: Get the total size of the hash map (sum of newMap and oldMap)
size_t hm_size(HMap *hmap)
{
    return hmap->newMap.size + hmap->oldMap.size;
}
