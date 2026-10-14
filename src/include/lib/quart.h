/*-------------------------------------------------------------------------
 *
 * quart.h
 *		QuART (Quick Adaptive Radix Tree) extension for radixtree.h
 *
 * This file contains the QuART stail fast-path insertion for the adaptive
 * radix tree template.  It is included by radixtree.h when RT_USE_QUART is
 * defined.
 *
 * The tree caches a fast pointer "fp" to the shift-0 node (the parent of the
 * leaves) of the 256-key group (key >> RT_SPAN) most recently reached via a
 * change_fp insert.  Each key is classified against that group:
 *
 * - FP_INSERT: same group -> insert directly into fp, no root-down descent
 * - BRIDGE:	adjacent group (+/-1) -> insert from the root and move fp
 * - OTHER:		any other group -> insert from the root, keeping fp, unless
 *				RT_QUART_RESET_COUNTER_INIT consecutive OTHER keys have
 *				been seen, in which case fp moves to the new group
 *
 * PostgreSQL's radix tree has no lazy expansion or prefix compression, so fp
 * is always a shift-0 node.  Inserts into other groups never modify fp, and
 * inner nodes are never moved by an insert into a child, so fp stays valid
 * across preserve_fp inserts.  fp is only replaced when it is freed (grown,
 * shrunk or deleted); RT_FREE_NODE then invalidates it, so mixing RT_SET and
 * RT_DELETE with RT_SET_QUART is safe.  Growing fp from the fast path takes
 * the root-down path and re-locates fp.
 *
 * Copyright (c) 2024-2026, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/include/lib/quart.h
 *
 *-------------------------------------------------------------------------
 */

#ifndef RT_USE_QUART
#error "quart.h should only be included when RT_USE_QUART is defined"
#endif

/* Number of consecutive OTHER keys tolerated before moving the fast path */
#define RT_QUART_RESET_COUNTER_INIT 16

/*
 * Return the shift-0 node on the path to "key", which must be in the tree.
 */
static inline RT_PTR_ALLOC
RT_QUART_LOCATE_FP(RT_RADIX_TREE * tree, uint64 key)
{
	RT_CHILD_PTR node;
	int			shift = tree->ctl->start_shift;

	node.alloc = tree->ctl->root;
	while (shift > 0)
	{
		RT_PTR_ALLOC *slot;

		RT_PTR_SET_LOCAL(tree, &node);
		slot = RT_NODE_SEARCH(node.local, RT_GET_KEY_CHUNK(key, shift));
		Assert(slot != NULL);
		node.alloc = *slot;
		shift -= RT_SPAN;
	}

	return node.alloc;
}

/*
 * Store *value_p into the leaf slot "slot", as RT_SET does.
 */
static inline void
RT_QUART_STORE_VALUE(RT_RADIX_TREE * tree, RT_PTR_ALLOC * slot, bool found,
					 RT_VALUE_TYPE * value_p)
{
	size_t		value_sz = RT_GET_VALUE_SIZE(value_p);

	if (RT_VALUE_IS_EMBEDDABLE(value_p))
	{
		/* free the existing leaf */
		if (found && !RT_CHILDPTR_IS_VALUE(*slot))
			RT_FREE_LEAF(tree, *slot);

		/* store value directly in child pointer slot */
		memcpy(slot, value_p, value_sz);

#ifdef RT_RUNTIME_EMBEDDABLE_VALUE
		/* tag child pointer */
#ifdef RT_SHMEM
		*slot |= 1;
#else
		*((uintptr_t *) slot) |= 1;
#endif
#endif
	}
	else
	{
		RT_CHILD_PTR leaf;

		if (found && !RT_CHILDPTR_IS_VALUE(*slot))
		{
			Assert(RT_PTR_ALLOC_IS_VALID(*slot));
			leaf.alloc = *slot;
			RT_PTR_SET_LOCAL(tree, &leaf);

			if (RT_GET_VALUE_SIZE((RT_VALUE_TYPE *) leaf.local) != value_sz)
			{
				/*
				 * different sizes, so first free the existing leaf before
				 * allocating a new one
				 */
				RT_FREE_LEAF(tree, *slot);
				leaf = RT_ALLOC_LEAF(tree, value_sz);
				*slot = leaf.alloc;
			}
		}
		else
		{
			/* allocate new leaf and store it in the child array */
			leaf = RT_ALLOC_LEAF(tree, value_sz);
			*slot = leaf.alloc;
		}

		memcpy(leaf.local, value_p, value_sz);
	}
}

/*
 * QuART stail insertion: same semantics as RT_SET, but keys in the cached
 * fast-path group skip the root-down traversal.  Mirrors QuART_stail::insert.
 *
 * Taking a lock in exclusive mode is the caller's responsibility.
 */
RT_SCOPE bool
RT_SET_QUART(RT_RADIX_TREE * tree, uint64 key, RT_VALUE_TYPE * value_p)
{
	uint64		group = key >> RT_SPAN;
	bool		found;

#ifdef RT_SHMEM
	Assert(tree->ctl->magic == RT_RADIX_TREE_MAGIC);
#endif

	/* No fast path yet (empty tree, or invalidated): establish one */
	if (!RT_PTR_ALLOC_IS_VALID(tree->ctl->fp))
		goto change_fp;

	if (group == tree->ctl->fp_group)
	{
		/* FP_INSERT: insert directly into the fp node */
		RT_CHILD_PTR node;
		RT_PTR_ALLOC *slot;
		uint8		chunk = RT_GET_KEY_CHUNK(key, 0);

		tree->ctl->fp_reset_counter = RT_QUART_RESET_COUNTER_INIT;

		node.alloc = tree->ctl->fp;
		RT_PTR_SET_LOCAL(tree, &node);
		slot = RT_NODE_SEARCH(node.local, chunk);

		if (slot != NULL)
			found = true;
		else if (node.local->kind == RT_NODE_KIND_256 ||
				 !RT_NODE_MUST_GROW(node.local))
		{
			/* no growth, so the parent slot is never touched */
			slot = RT_NODE_INSERT(tree, NULL, node, chunk);
			found = false;
		}
		else
		{
			/*
			 * fp must grow, which replaces it in its parent.  We don't cache
			 * the parent slot (inserts into sibling groups can move or
			 * reallocate it), so take the root-down path and re-locate fp.
			 */
			goto change_fp;
		}

		RT_QUART_STORE_VALUE(tree, slot, found, value_p);
		if (!found)
			tree->ctl->num_keys++;
		return found;
	}

	if (group + 1 == tree->ctl->fp_group || tree->ctl->fp_group + 1 == group)
	{
		/* BRIDGE: adjacent group, move the fast path */
		tree->ctl->fp_reset_counter = RT_QUART_RESET_COUNTER_INIT;
		goto change_fp;
	}

	/* OTHER */
	if (tree->ctl->fp_reset_counter > 0)
	{
		/* preserve_fp: insert from the root and keep the fast path */
		tree->ctl->fp_reset_counter--;
		return RT_SET(tree, key, value_p);
	}
	tree->ctl->fp_reset_counter = RT_QUART_RESET_COUNTER_INIT;

change_fp:
	/* insert from the root, then point the fast path at key's group */
	found = RT_SET(tree, key, value_p);
	tree->ctl->fp = RT_QUART_LOCATE_FP(tree, key);
	tree->ctl->fp_group = group;
	return found;
}

/*
 * RT_SET_QUART_BULK
 *
 * Bulk-load a CONTIGUOUS key range (keys[i] == keys[0] + i) at any offset.
 * The 256-aligned interior is built one full node256 leaf at a time by filling
 * the children[] array directly (embedded values), rather than descending from
 * the root per key; the (<256) leading and trailing partials go through RT_SET.
 * This is the PostgreSQL-radix-tree analogue of ART::bulkLoadRange.
 *
 * Requires embeddable values (value size <= pointer size), which the VACUUM
 * uint64 block-number workload satisfies.
 */
RT_SCOPE void
RT_SET_QUART_BULK(RT_RADIX_TREE *tree, uint64 *keys, RT_VALUE_TYPE *values, size_t n)
{
	size_t		firstAligned,
				g,
				endAligned,
				k;

	if (n == 0)
		return;

	/* leaf nodes are spliced in below without RT_FREE_NODE: drop the fast path */
	tree->ctl->fp = RT_INVALID_PTR_ALLOC;

	/* Bootstrap the tree, then extend it upward to cover the whole range. */
	RT_SET(tree, keys[0], &values[0]);
	if (keys[n - 1] > tree->ctl->max_val)
		RT_EXTEND_UP(tree, keys[n - 1]);

	/*
	 * If the tree is too shallow to have a shift-8 parent above the leaves
	 * (max key < 256), there is no aligned interior worth special-casing;
	 * fall back to ordinary insertion for every key.
	 */
	if (tree->ctl->start_shift < RT_SPAN)
	{
		for (k = 1; k < n; k++)
			RT_SET(tree, keys[k], &values[k]);
		tree->ctl->num_keys = n;
		return;
	}

	/* first index whose value is 256-aligned (low chunk == 0) */
	firstAligned = 0;
	while (firstAligned < n && (keys[firstAligned] & RT_CHUNK_MASK) != 0)
		firstAligned++;

	/* Build each complete 256-aligned group as a single node256 leaf. */
	for (g = firstAligned; g + RT_NODE_MAX_SLOTS <= n; g += RT_NODE_MAX_SLOTS)
	{
		uint64		V = keys[g];	/* (V & RT_CHUNK_MASK) == 0 */
		RT_PTR_ALLOC *parent_slot = &tree->ctl->root;
		RT_PTR_ALLOC *slot;
		RT_CHILD_PTR leaf;
		RT_NODE_256 *n256;
		int			shift = tree->ctl->start_shift;
		int			j;

		/* Ensure the whole path down to shift 0 exists for V. */
		RT_SET(tree, V, &values[g]);

		/* Navigate existing nodes down to the shift-8 parent of the leaf. */
		while (shift > RT_SPAN)
		{
			RT_CHILD_PTR node;

			node.alloc = *parent_slot;
			RT_PTR_SET_LOCAL(tree, &node);
			parent_slot = RT_NODE_SEARCH(node.local, RT_GET_KEY_CHUNK(V, shift));
			shift -= RT_SPAN;
		}
		{
			RT_CHILD_PTR node8;

			node8.alloc = *parent_slot;
			RT_PTR_SET_LOCAL(tree, &node8);
			slot = RT_NODE_SEARCH(node8.local, RT_GET_KEY_CHUNK(V, RT_SPAN));
		}

		/* Build a full node256 leaf and splice it over the shift-0 node. */
		leaf = RT_ALLOC_NODE(tree, RT_NODE_KIND_256, RT_CLASS_256);
		n256 = (RT_NODE_256 *) leaf.local;
		for (j = 0; j < RT_NODE_MAX_SLOTS; j++)
		{
			memcpy(&n256->children[j], &values[g + j], sizeof(RT_VALUE_TYPE));
			n256->isset[RT_BM_IDX(j)] |= ((bitmapword) 1 << RT_BM_BIT(j));
		}
		/* base.count stays 0: a fully-populated node256 overflows to 0 and the
		 * node256 code never introspects count (lookups use the isset bitmap). */
		*slot = leaf.alloc;
	}
	endAligned = g;

	/* ragged ends via ordinary insertion */
	for (k = 0; k < firstAligned; k++)
		RT_SET(tree, keys[k], &values[k]);
	for (k = endAligned; k < n; k++)
		RT_SET(tree, keys[k], &values[k]);

	tree->ctl->num_keys = n;
}
