/*-------------------------------------------------------------------------
 *
 * quart.h
 *		QuART (Quick Adaptive Radix Tree) extension for radixtree.h
 *
 * This file contains the QuART stail fast-path insertion and grouped bulk
 * loading for the adaptive radix tree template.  It is included by
 * radixtree.h when RT_USE_QUART is defined.
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
 * Return the slot holding the shift-RT_SPAN node on the path to "key" (the
 * parent of key's shift-0 node), creating any missing nodes above it.  This
 * is getOrCreatePath() of the bulk loader.  Requires start_shift >= RT_SPAN.
 */
static RT_PTR_ALLOC *
RT_QUART_BULK_PATH(RT_RADIX_TREE * tree, uint64 key)
{
	RT_PTR_ALLOC *parent_slot = &tree->ctl->root;
	int			shift = tree->ctl->start_shift;

	while (shift > RT_SPAN)
	{
		RT_CHILD_PTR node;
		RT_PTR_ALLOC *slot;
		uint8		chunk = RT_GET_KEY_CHUNK(key, shift);

		node.alloc = *parent_slot;
		RT_PTR_SET_LOCAL(tree, &node);
		slot = RT_NODE_SEARCH(node.local, chunk);
		if (slot == NULL)
		{
			slot = RT_NODE_INSERT(tree, parent_slot, node, chunk);
			*slot = RT_ALLOC_NODE(tree, RT_NODE_KIND_4, RT_CLASS_4).alloc;
		}
		parent_slot = slot;
		shift -= RT_SPAN;
	}

	return parent_slot;
}

/*
 * Build the shift-0 node for the group keys[0..m-1] (1 <= m <= 256 sorted,
 * unique keys sharing key >> RT_SPAN) at its final size: the smallest node
 * kind and size class that holds m children, filled in chunk order.
 */
static RT_PTR_ALLOC
RT_QUART_BULK_GROUP(RT_RADIX_TREE * tree, uint64 *keys, RT_VALUE_TYPE * values,
					size_t m)
{
	RT_CHILD_PTR node;

	if (m <= RT_FANOUT_4)
	{
		RT_NODE_4  *n4;

		node = RT_ALLOC_NODE(tree, RT_NODE_KIND_4, RT_CLASS_4);
		n4 = (RT_NODE_4 *) node.local;
		for (size_t k = 0; k < m; k++)
		{
			n4->chunks[k] = RT_GET_KEY_CHUNK(keys[k], 0);
			RT_QUART_STORE_VALUE(tree, &n4->children[k], false, &values[k]);
		}
	}
	else if (m <= RT_FANOUT_16_HI)
	{
		RT_NODE_16 *n16;

		node = RT_ALLOC_NODE(tree, RT_NODE_KIND_16,
							 m <= RT_FANOUT_16_LO ? RT_CLASS_16_LO : RT_CLASS_16_HI);
		n16 = (RT_NODE_16 *) node.local;
		for (size_t k = 0; k < m; k++)
		{
			n16->chunks[k] = RT_GET_KEY_CHUNK(keys[k], 0);
			RT_QUART_STORE_VALUE(tree, &n16->children[k], false, &values[k]);
		}
	}
	else if (m <= RT_FANOUT_48)
	{
		RT_NODE_48 *n48;

		node = RT_ALLOC_NODE(tree, RT_NODE_KIND_48, RT_CLASS_48);
		n48 = (RT_NODE_48 *) node.local;
		for (size_t k = 0; k < m; k++)
		{
			n48->slot_idxs[RT_GET_KEY_CHUNK(keys[k], 0)] = k;
			n48->isset[RT_BM_IDX(k)] |= ((bitmapword) 1 << RT_BM_BIT(k));
			RT_QUART_STORE_VALUE(tree, &n48->children[k], false, &values[k]);
		}
	}
	else
	{
		RT_NODE_256 *n256;

		node = RT_ALLOC_NODE(tree, RT_NODE_KIND_256, RT_CLASS_256);
		n256 = (RT_NODE_256 *) node.local;
		if (m == RT_NODE_MAX_SLOTS)
		{
			/* full group: the chunk of keys[k] is k */
			memset(n256->isset, 0xFF, sizeof(n256->isset));
			for (size_t k = 0; k < m; k++)
				RT_QUART_STORE_VALUE(tree, &n256->children[k], false, &values[k]);
		}
		else
		{
			for (size_t k = 0; k < m; k++)
			{
				uint8		chunk = RT_GET_KEY_CHUNK(keys[k], 0);

				n256->isset[RT_BM_IDX(chunk)] |= ((bitmapword) 1 << RT_BM_BIT(chunk));
				RT_QUART_STORE_VALUE(tree, &n256->children[chunk], false, &values[k]);
			}
		}
	}

	/* overflows to 0 for a full node256, as the node256 code expects */
	node.local->count = m;
	RT_VERIFY_NODE(node.local);

	return node.alloc;
}

/*
 * RT_SET_QUART_BULK
 *
 * Grouped bulk loading of a sorted key array (paper §3.4, Algorithm 2), the
 * PostgreSQL-radix-tree analogue of ART::bulkLoad.  Keys must be strictly
 * ascending; gaps and arbitrary offsets are allowed.
 *
 * A group is the maximal run of keys sharing key >> RT_SPAN; its 1..256 keys
 * differ only in the shift-0 chunk, so the group's shift-0 node is built
 * directly at its final size and attached under "cursor", the shift-RT_SPAN
 * node for the current key >> (2 * RT_SPAN) prefix, with no per-key root-down
 * descent:
 *
 * - BL_FP_INSERT: same prefix as the previous group -> stay under cursor
 * - BL_BRIDGE:	new prefix (or first group) -> getOrCreatePath moves cursor
 *
 * Since the input is sorted, a prefix is never revisited once left, so the
 * slot holding cursor stays valid until the next bridge.  PostgreSQL's radix
 * tree has no lazy expansion, so a single-key group gets a node4 like RT_SET
 * would create.  If the tree already has a shift-0 node for a group, that
 * group falls back to RT_SET.  Existing shift-0 nodes are never replaced, so
 * the stail fast path stays valid.
 */
RT_SCOPE void
RT_SET_QUART_BULK(RT_RADIX_TREE * tree, uint64 *keys, RT_VALUE_TYPE * values, size_t n)
{
	RT_PTR_ALLOC *cursor_slot = NULL;
	uint64		cursor_prefix = 0;
	size_t		i,
				j;

#ifdef RT_SHMEM
	Assert(tree->ctl->magic == RT_RADIX_TREE_MAGIC);
#endif

	if (n == 0)
		return;

	/* Make the tree tall enough for the largest key. */
	if (keys[n - 1] > tree->ctl->max_val)
	{
		if (tree->ctl->num_keys == 0)
		{
			/* as in RT_SET, reuse the empty root rather than extending up */
			tree->ctl->start_shift = RT_KEY_GET_SHIFT(keys[n - 1]);
			tree->ctl->max_val = RT_SHIFT_GET_MAX_VAL(tree->ctl->start_shift);
		}
		else
			RT_EXTEND_UP(tree, keys[n - 1]);
	}

	/* every key is below 256: a single group in the root, nothing to gain */
	if (tree->ctl->start_shift < RT_SPAN)
	{
		for (i = 0; i < n; i++)
			RT_SET(tree, keys[i], &values[i]);
		return;
	}

	for (i = 0; i < n; i = j)
	{
		uint64		group = keys[i] >> RT_SPAN;
		uint8		chunk = RT_GET_KEY_CHUNK(keys[i], RT_SPAN);
		RT_CHILD_PTR cursor;

		/* the group is keys[i..j); a full one is detected in O(1) */
		if (i + RT_NODE_MAX_SLOTS <= n &&
			keys[i + RT_NODE_MAX_SLOTS - 1] >> RT_SPAN == group)
			j = i + RT_NODE_MAX_SLOTS;
		else
			for (j = i + 1; j < n && keys[j] >> RT_SPAN == group; j++)
				;

#ifdef USE_ASSERT_CHECKING
		for (size_t k = i + 1; k < j; k++)
			Assert(keys[k - 1] < keys[k]);
		Assert(j == n || keys[j - 1] < keys[j]);
#endif

		/* BL_BRIDGE: move cursor to the new prefix */
		if (cursor_slot == NULL || group >> RT_SPAN != cursor_prefix)
		{
			cursor_slot = RT_QUART_BULK_PATH(tree, keys[i]);
			cursor_prefix = group >> RT_SPAN;
		}

		/* BL_FP_INSERT (and after a bridge): hang the group under cursor */
		cursor.alloc = *cursor_slot;
		RT_PTR_SET_LOCAL(tree, &cursor);
		if (RT_NODE_SEARCH(cursor.local, chunk) == NULL)
		{
			RT_PTR_ALLOC groupnode = RT_QUART_BULK_GROUP(tree, &keys[i],
														 &values[i], j - i);

			*RT_NODE_INSERT(tree, cursor_slot, cursor, chunk) = groupnode;
			tree->ctl->num_keys += j - i;
		}
		else
		{
			/* the group's node already exists (non-empty tree) */
			for (size_t k = i; k < j; k++)
				RT_SET(tree, keys[k], &values[k]);
		}
	}
}
