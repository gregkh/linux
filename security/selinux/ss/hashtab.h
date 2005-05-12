/*
 * A hash table (hashtab) maintains associations between
 * key values and datum values.  The type of the key values
 * and the type of the datum values is arbitrary.  The
 * functions for hash computation and key comparison are
 * provided by the creator of the table.
 *
 * Author : Stephen Smalley, <sds@epoch.ncsc.mil>
 */
#ifndef _SS_HASHTAB_H_
#define _SS_HASHTAB_H_

#define HASHTAB_MAX_NODES	0xffffffff

struct hashtab_node {
	void *key;
	void *datum;
	struct hashtab_node *next;
};

struct hashtab {
	struct hashtab_node **htable;	/* hash table */
	u32 size;			/* number of slots in hash table */
	u32 nel;			/* number of elements in hash table */
	u32 (*hash_value)(struct hashtab *h, void *key);
					/* hash function */
	int (*keycmp)(struct hashtab *h, void *key1, void *key2);
					/* key comparison function */
};

struct hashtab_info {
	u32 slots_used;
	u32 max_chain_len;
};

/*
 * Creates a new hash table with the specified characteristics.
 *
 * Returns NULL if insufficent space is available or
 * the new hash table otherwise.
 */
struct hashtab *hashtab_create(u32 (*hash_value)(struct hashtab *h, void *key),
                               int (*keycmp)(struct hashtab *h, void *key1, void *key2),
                               u32 size);

/*
 * Inserts the specified (key, datum) pair into the specified hash table.
 *
 * Returns -ENOMEM on memory allocation error,
 * -EEXIST if there is already an entry with the same key,
 * -EINVAL for general errors or
 * 0 otherwise.
 */
int hashtab_insert(struct hashtab *h, void *k, void *d);

/*
 * Removes the entry with the specified key from the hash table.
 * Applies the specified destroy function to (key,datum,args) for
 * the entry.
 *
 * Returns -ENOENT if no entry has the specified key,
 * -EINVAL for general errors or
 *0 otherwise.
 */
int hashtab_remove(struct hashtab *h, void *k,
		   void (*destroy)(void *k, void *d, void *args),
		   void *args);

/*
 * Insert or replace the specified (key, datum) pair in the specified
 * hash table.  If an entry for the specified key already exists,
 * then the specified destroy function is applied to (key,datum,args)
 * for the entry prior to replacing the entry's contents.
 *
 * Returns -ENOMEM if insufficient space is available,
 * -EINVAL for general errors or
 * 0 otherwise.
 */
int hashtab_replace(struct hashtab *h, void *k, void *d,
		    void (*destroy)(void *k, void *d, void *args),
		    void *args);

/*
 * Searches for the entry with the specified key in the hash table.
 *
 * Returns NULL if no entry has the specified key or
 * the datum of the entry otherwise.
 */
void *hashtab_search(struct hashtab *h, void *k);

/*
 * Destroys the specified hash table.
 */
void hashtab_destroy(struct hashtab *h);

/*
 * Applies the specified apply function to (key,datum,args)
 * for each entry in the specified hash table.
 *
 * The order in which the function is applied to the entries
 * is dependent upon the internal structure of the hash table.
 *
 * If apply returns a non-zero status, then hashtab_map will cease
 * iterating through the hash table and will propagate the error
 * return to its caller.
 */
int hashtab_map(struct hashtab *h,
		int (*apply)(void *k, void *d, void *args),
		void *args);

/*
 * Same as hashtab_map, except that if apply returns a non-zero status,
 * then the (key,datum) pair will be removed from the hashtab and the
 * destroy function will be applied to (key,datum,args).
 */
void hashtab_map_remove_on_error(struct hashtab *h,
                                 int (*apply)(void *k, void *d, void *args),
                                 void (*destroy)(void *k, void *d, void *args),
                                 void *args);


/* Fill info with some hash table statistics */
void hashtab_stat(struct hashtab *h, struct hashtab_info *info);

#endif	/* _SS_HASHTAB_H */
