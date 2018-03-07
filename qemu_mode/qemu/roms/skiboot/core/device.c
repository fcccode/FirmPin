/* Copyright 2013-2014 IBM Corp.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * 	http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or
 * implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <device.h>
#include <stdlib.h>
#include <skiboot.h>
#include <libfdt/libfdt.h>
#include <libfdt/libfdt_internal.h>
#include <ccan/str/str.h>
#include <ccan/endian/endian.h>

/* Used to give unique handles. */
u32 last_phandle = 0;

struct dt_node *dt_root;
struct dt_node *dt_chosen;

static const char *take_name(const char *name)
{
	if (!is_rodata(name) && !(name = strdup(name))) {
		prerror("Failed to allocate copy of name");
		abort();
	}
	return name;
}

static void free_name(const char *name)
{
	if (!is_rodata(name))
		free((char *)name);
}

static struct dt_node *new_node(const char *name)
{
	struct dt_node *node = malloc(sizeof *node);
	if (!node) {
		prerror("Failed to allocate node\n");
		abort();
	}

	node->name = take_name(name);
	node->parent = NULL;
	list_head_init(&node->properties);
	list_head_init(&node->children);
	/* FIXME: locking? */
	node->phandle = ++last_phandle;
	return node;
}

struct dt_node *dt_new_root(const char *name)
{
	return new_node(name);
}

static const char *get_unitname(const struct dt_node *node)
{
	const char *c = strchr(node->name, '@');

	if (!c)
		return NULL;

	return c + 1;
}

int dt_cmp_subnodes(const struct dt_node *a, const struct dt_node *b)
{
	const char *a_unit = get_unitname(a);
	const char *b_unit = get_unitname(b);

	ptrdiff_t basenamelen = a_unit - a->name;

	/* sort hex unit addresses by number */
	if (a_unit && b_unit && !strncmp(a->name, b->name, basenamelen)) {
		unsigned long long a_num, b_num;
		char *a_end, *b_end;

		a_num = strtoul(a_unit, &a_end, 16);
		b_num = strtoul(b_unit, &b_end, 16);

		/* only compare if the unit addr parsed correctly */
		if (*a_end == 0 && *b_end == 0)
			return (a_num > b_num) - (a_num < b_num);
	}

	return strcmp(a->name, b->name);
}

bool dt_attach_root(struct dt_node *parent, struct dt_node *root)
{
	struct dt_node *node;

	assert(!root->parent);

	if (list_empty(&parent->children)) {
		list_add(&parent->children, &root->list);
		root->parent = parent;

		return true;
	}

	dt_for_each_child(parent, node) {
		int cmp = dt_cmp_subnodes(node, root);

		/* Look for duplicates */
		if (cmp == 0) {
			prerror("DT: %s failed, duplicate %s\n",
				__func__, root->name);
			return false;
		}

		/* insert before the first node that's larger
		 * the the node we're inserting */
		if (cmp > 0)
			break;
	}

	list_add_before(&parent->children, &root->list, &node->list);
	root->parent = parent;

	return true;
}

static inline void dt_destroy(struct dt_node *dn)
{
	if (!dn)
		return;

	free_name(dn->name);
	free(dn);
}
	
struct dt_node *dt_new(struct dt_node *parent, const char *name)
{
	struct dt_node *new;
	assert(parent);

	new = new_node(name);
	if (!dt_attach_root(parent, new)) {
		dt_destroy(new);
		return NULL;
	}
	return new;
}

struct dt_node *dt_new_addr(struct dt_node *parent, const char *name,
			    uint64_t addr)
{
	char *lname;
	struct dt_node *new;
	size_t len;

	assert(parent);
	len = strlen(name) + STR_MAX_CHARS(addr) + 2;
	lname = malloc(len);
	if (!lname)
		return NULL;
	snprintf(lname, len, "%s@%llx", name, (long long)addr);
	new = new_node(lname);
	free(lname);
	if (!dt_attach_root(parent, new)) {
		dt_destroy(new);
		return NULL;
	}
	return new;
}

struct dt_node *dt_new_2addr(struct dt_node *parent, const char *name,
			     uint64_t addr0, uint64_t addr1)
{
	char *lname;
	struct dt_node *new;
	size_t len;
	assert(parent);

	len = strlen(name) + 2*STR_MAX_CHARS(addr0) + 3;
	lname = malloc(len);
	if (!lname)
		return NULL;
	snprintf(lname, len, "%s@%llx,%llx",
		 name, (long long)addr0, (long long)addr1);
	new = new_node(lname);
	free(lname);
	if (!dt_attach_root(parent, new)) {
		dt_destroy(new);
		return NULL;
	}
	return new;
}

static struct dt_node *__dt_copy(struct dt_node *node, struct dt_node *parent,
		bool root)
{
	struct dt_property *prop, *new_prop;
	struct dt_node *new_node, *child;

	new_node = dt_new(parent, node->name);
	if (!new_node)
		return NULL;

	list_for_each(&node->properties, prop, list) {
		new_prop = dt_add_property(new_node, prop->name, prop->prop,
				prop->len);
		if (!new_prop)
			goto fail;
	}

	list_for_each(&node->children, child, list) {
		child = __dt_copy(child, new_node, false);
		if (!child)
			goto fail;
	}

	return new_node;

fail:
	/* dt_free will recurse for us, so only free when we unwind to the
	 * top-level failure */
	if (root)
		dt_free(new_node);
	return NULL;
}

struct dt_node *dt_copy(struct dt_node *node, struct dt_node *parent)
{
	return __dt_copy(node, parent, true);
}

char *dt_get_path(const struct dt_node *node)
{
	unsigned int len = 0;
	const struct dt_node *n;
	char *path, *p;

	/* Dealing with NULL is for test/debug purposes */
	if (!node)
		return strdup("<NULL>");

	for (n = node; n; n = n->parent) {
		len += strlen(n->name);
		if (n->parent || n == node)
			len++;
	}
	path = zalloc(len + 1);
	assert(path);
	p = path + len;
	for (n = node; n; n = n->parent) {
		len = strlen(n->name);
		p -= len;
		memcpy(p, n->name, len);
		if (n->parent || n == node)
			*(--p) = '/';
	}
	assert(p == path);

	return p;
}

static const char *__dt_path_split(const char *p,
				   const char **namep, unsigned int *namel,
				   const char **addrp, unsigned int *addrl)
{
	const char *at, *sl;

	*namel = *addrl = 0;

	/* Skip initial '/' */
	while (*p == '/')
		p++;

	/* Check empty path */
	if (*p == 0)
		return p;

	at = strchr(p, '@');
	sl = strchr(p, '/');
	if (sl == NULL)
		sl = p + strlen(p);
	if (sl < at)
		at = NULL;
	if (at) {
		*addrp = at + 1;
		*addrl = sl - at - 1;
	}
	*namep = p;
	*namel = at ? (at - p) : (sl - p);

	return sl;
}

struct dt_node *dt_find_by_path(struct dt_node *root, const char *path)
{
	struct dt_node *n;
	const char *pn, *pa, *p = path, *nn, *na;
	unsigned int pnl, pal, nnl, nal;
	bool match;

	/* Walk path components */
	while (*p) {
		/* Extract next path component */
		p = __dt_path_split(p, &pn, &pnl, &pa, &pal);
		if (pnl == 0 && pal == 0)
			break;

		/* Compare with each child node */
		match = false;
		list_for_each(&root->children, n, list) {
			match = true;
			__dt_path_split(n->name, &nn, &nnl, &na, &nal);
			if (pnl && (pnl != nnl || strncmp(pn, nn, pnl)))
				match = false;
			if (pal && (pal != nal || strncmp(pa, na, pal)))
				match = false;
			if (match) {
				root = n;
				break;
			}
		}

		/* No child match */
		if (!match)
			return NULL;
	}
	return root;
}

struct dt_node *dt_find_by_name(struct dt_node *root, const char *name)
{
	struct dt_node *child, *match;

	list_for_each(&root->children, child, list) {
		if (!strcmp(child->name, name))
			return child;

		match = dt_find_by_name(child, name);
		if (match)
			return match;
	}

	return NULL;
}

struct dt_node *dt_find_by_phandle(struct dt_node *root, u32 phandle)
{
	struct dt_node *node;

	dt_for_each_node(root, node)
		if (node->phandle == phandle)
			return node;
	return NULL;
}

static struct dt_property *new_property(struct dt_node *node,
					const char *name, size_t size)
{
	struct dt_property *p = malloc(sizeof(*p) + size);
	char *path;

	if (!p) {
		path = dt_get_path(node);
		prerror("Failed to allocate property \"%s\" for %s of %zu bytes\n",
			name, path, size);
		free(path);
		abort();
	}
	if (dt_find_property(node, name)) {
		path = dt_get_path(node);
		prerror("Duplicate property \"%s\" in node %s\n",
			name, path);
		free(path);
		abort();

	}

	p->name = take_name(name);
	p->len = size;
	list_add_tail(&node->properties, &p->list);
	return p;
}

struct dt_property *dt_add_property(struct dt_node *node,
				    const char *name,
				    const void *val, size_t size)
{
	struct dt_property *p;

	/*
	 * Filter out phandle properties, we re-generate them
	 * when flattening
	 */
	if (strcmp(name, "linux,phandle") == 0 ||
	    strcmp(name, "phandle") == 0) {
		assert(size == 4);
		node->phandle = *(const u32 *)val;
		if (node->phandle >= last_phandle)
			last_phandle = node->phandle;
		return NULL;
	}

	p = new_property(node, name, size);
	if (size)
		memcpy(p->prop, val, size);
	return p;
}

void dt_resize_property(struct dt_property **prop, size_t len)
{
	size_t new_len = sizeof(**prop) + len;

	*prop = realloc(*prop, new_len);

	/* Fix up linked lists in case we moved. (note: not an empty list). */
	(*prop)->list.next->prev = &(*prop)->list;
	(*prop)->list.prev->next = &(*prop)->list;
}

struct dt_property *dt_add_property_string(struct dt_node *node,
					   const char *name,
					   const char *value)
{
	return dt_add_property(node, name, value, strlen(value)+1);
}

struct dt_property *dt_add_property_nstr(struct dt_node *node,
					 const char *name,
					 const char *value, unsigned int vlen)
{
	struct dt_property *p;
	char *tmp = zalloc(vlen + 1);

	if (!tmp)
		return NULL;

	strncpy(tmp, value, vlen);
	p = dt_add_property(node, name, tmp, strlen(tmp)+1);
	free(tmp);

	return p;
}

struct dt_property *__dt_add_property_cells(struct dt_node *node,
					    const char *name,
					    int count, ...)
{
	struct dt_property *p;
	u32 *val;
	unsigned int i;
	va_list args;

	p = new_property(node, name, count * sizeof(u32));
	val = (u32 *)p->prop;
	va_start(args, count);
	for (i = 0; i < count; i++)
		val[i] = cpu_to_fdt32(va_arg(args, u32));
	va_end(args);
	return p;
}

struct dt_property *__dt_add_property_u64s(struct dt_node *node,
					   const char *name,
					   int count, ...)
{
	struct dt_property *p;
	u64 *val;
	unsigned int i;
	va_list args;

	p = new_property(node, name, count * sizeof(u64));
	val = (u64 *)p->prop;
	va_start(args, count);
	for (i = 0; i < count; i++)
		val[i] = cpu_to_fdt64(va_arg(args, u64));
	va_end(args);
	return p;
}

struct dt_property *__dt_add_property_strings(struct dt_node *node,
					      const char *name,
					      int count, ...)
{
	struct dt_property *p;
	unsigned int i, size;
	va_list args;
	const char *sstr;
	char *s;

	va_start(args, count);
	for (i = size = 0; i < count; i++) {
		sstr = va_arg(args, const char *);
		if (sstr)
			size += strlen(sstr) + 1;
	}
	va_end(args);
	if (!size)
		size = 1;
	p = new_property(node, name, size);
	s = (char *)p->prop;
	*s = 0;
	va_start(args, count);
	for (i = 0; i < count; i++) {	
		sstr = va_arg(args, const char *);
		if (sstr) {
			strcpy(s, sstr);
			s = s + strlen(sstr) + 1;
		}
	}
	va_end(args);
	return p;
}

void dt_del_property(struct dt_node *node, struct dt_property *prop)
{
	list_del_from(&node->properties, &prop->list);
	free_name(prop->name);
	free(prop);
}

u32 dt_property_get_cell(const struct dt_property *prop, u32 index)
{
	assert(prop->len >= (index+1)*sizeof(u32));
	/* Always aligned, so this works. */
	return fdt32_to_cpu(((const u32 *)prop->prop)[index]);
}

/* First child of this node. */
struct dt_node *dt_first(const struct dt_node *root)
{
	return list_top(&root->children, struct dt_node, list);
}

/* Return next node, or NULL. */
struct dt_node *dt_next(const struct dt_node *root,
			const struct dt_node *prev)
{
	/* Children? */
	if (!list_empty(&prev->children))
		return dt_first(prev);

	do {
		/* More siblings? */
		if (prev->list.next != &prev->parent->children.n)
			return list_entry(prev->list.next, struct dt_node,list);

		/* No more siblings, move up to parent. */
		prev = prev->parent;
	} while (prev != root);

	return NULL;
}

struct dt_property *__dt_find_property(struct dt_node *node, const char *name)
{
	struct dt_property *i;

	list_for_each(&node->properties, i, list)
		if (strcmp(i->name, name) == 0)
			return i;
	return NULL;
}

const struct dt_property *dt_find_property(const struct dt_node *node,
					   const char *name)
{
	const struct dt_property *i;

	list_for_each(&node->properties, i, list)
		if (strcmp(i->name, name) == 0)
			return i;
	return NULL;
}

const struct dt_property *dt_require_property(const struct dt_node *node,
					      const char *name, int wanted_len)
{
	const struct dt_property *p = dt_find_property(node, name);

	if (!p) {
		const char *path = dt_get_path(node);

		prerror("DT: Missing required property %s/%s\n",
			path, name);
		assert(false);
	}
	if (wanted_len >= 0 && p->len != wanted_len) {
		const char *path = dt_get_path(node);

		prerror("DT: Unexpected property length %s/%s\n",
			path, name);
		prerror("DT: Expected len: %d got len: %zu\n",
			wanted_len, p->len);
		assert(false);
	}

	return p;
}

bool dt_has_node_property(const struct dt_node *node,
			  const char *name, const char *val)
{
	const struct dt_property *p = dt_find_property(node, name);

	if (!p)
		return false;
	if (!val)
		return true;

	return p->len == strlen(val) + 1 && memcmp(p->prop, val, p->len) == 0;
}

bool dt_prop_find_string(const struct dt_property *p, const char *s)
{
	const char *c, *end;

	if (!p)
		return false;
	c = p->prop;
	end = c + p->len;

	while(c < end) {
		if (!strcasecmp(s, c))
			return true;
		c += strlen(c) + 1;
	}
	return false;
}

bool dt_node_is_compatible(const struct dt_node *node, const char *compat)
{
	const struct dt_property *p = dt_find_property(node, "compatible");

	return dt_prop_find_string(p, compat);
}

struct dt_node *dt_find_compatible_node(struct dt_node *root,
					struct dt_node *prev,
					const char *compat)
{
	struct dt_node *node;

	node = prev ? dt_next(root, prev) : root;
	for (; node; node = dt_next(root, node))
		if (dt_node_is_compatible(node, compat))
			return node;
	return NULL;
}

u64 dt_prop_get_u64(const struct dt_node *node, const char *prop)
{
	const struct dt_property *p = dt_require_property(node, prop, 8);

	return ((u64)dt_property_get_cell(p, 0) << 32)
		| dt_property_get_cell(p, 1);
}

u64 dt_prop_get_u64_def(const struct dt_node *node, const char *prop, u64 def)
{
	const struct dt_property *p = dt_find_property(node, prop);

	if (!p)
		return def;

	return ((u64)dt_property_get_cell(p, 0) << 32)
		| dt_property_get_cell(p, 1);
}

u32 dt_prop_get_u32(const struct dt_node *node, const char *prop)
{
	const struct dt_property *p = dt_require_property(node, prop, 4);

	return dt_property_get_cell(p, 0);
}

u32 dt_prop_get_u32_def(const struct dt_node *node, const char *prop, u32 def)
{
	const struct dt_property *p = dt_find_property(node, prop);

	if (!p)
		return def;

	return dt_property_get_cell(p, 0);
}

const void *dt_prop_get(const struct dt_node *node, const char *prop)
{
	const struct dt_property *p = dt_require_property(node, prop, -1);

	return p->prop;
}

const void *dt_prop_get_def(const struct dt_node *node, const char *prop,
			    void *def)
{
	const struct dt_property *p = dt_find_property(node, prop);

	return p ? p->prop : def;
}

const void *dt_prop_get_def_size(const struct dt_node *node, const char *prop,
				void *def, size_t *len)
{
	const struct dt_property *p = dt_find_property(node, prop);
	*len = 0;
	if (p)
		*len = p->len;

	return p ? p->prop : def;
}

u32 dt_prop_get_cell(const struct dt_node *node, const char *prop, u32 cell)
{
	const struct dt_property *p = dt_require_property(node, prop, -1);

	return dt_property_get_cell(p, cell);
}

u32 dt_prop_get_cell_def(const struct dt_node *node, const char *prop,
			 u32 cell, u32 def)
{
	const struct dt_property *p = dt_find_property(node, prop);

	if (!p)
		return def;

	return dt_property_get_cell(p, cell);
}

void dt_free(struct dt_node *node)
{
	struct dt_node *child;
	struct dt_property *p;

	while ((child = list_top(&node->children, struct dt_node, list)))
		dt_free(child);

	while ((p = list_pop(&node->properties, struct dt_property, list))) {
		free_name(p->name);
		free(p);
	}

	if (node->parent)
		list_del_from(&node->parent->children, &node->list);
	dt_destroy(node);
}

int dt_expand_node(struct dt_node *node, const void *fdt, int fdt_node)
{
	const struct fdt_property *prop;
	int offset, nextoffset, err;
	struct dt_node *child;
	const char *name;
	uint32_t tag;

	if (((err = fdt_check_header(fdt)) != 0)
	    || ((err = _fdt_check_node_offset(fdt, fdt_node)) < 0)) {
		prerror("FDT: Error %d parsing node 0x%x\n", err, fdt_node);
		return -1;
	}

	nextoffset = err;
	do {
		offset = nextoffset;

		tag = fdt_next_tag(fdt, offset, &nextoffset);
		switch (tag) {
		case FDT_PROP:
			prop = _fdt_offset_ptr(fdt, offset);
			name = fdt_string(fdt, fdt32_to_cpu(prop->nameoff));
			dt_add_property(node, name, prop->data,
					fdt32_to_cpu(prop->len));
			break;
		case FDT_BEGIN_NODE:
			name = fdt_get_name(fdt, offset, NULL);
			child = dt_new_root(name);
			assert(child);
			nextoffset = dt_expand_node(child, fdt, offset);

			/*
			 * This may fail in case of duplicate, keep it
			 * going for now, we may ultimately want to
			 * assert
			 */
			(void)dt_attach_root(node, child);
			break;
		case FDT_END:
			return -1;
		}
	} while (tag != FDT_END_NODE);

	return nextoffset;
}

void dt_expand(const void *fdt)
{
	printf("FDT: Parsing fdt @%p\n", fdt);

	dt_root = dt_new_root("");

	if (dt_expand_node(dt_root, fdt, 0) < 0)
		abort();
}

u64 dt_get_number(const void *pdata, unsigned int cells)
{
	const u32 *p = pdata;
	u64 ret = 0;

	while(cells--)
		ret = (ret << 32) | be32_to_cpu(*(p++));
	return ret;
}

u32 dt_n_address_cells(const struct dt_node *node)
{
	if (!node->parent)
		return 0;
	return dt_prop_get_u32_def(node->parent, "#address-cells", 2);
}

u32 dt_n_size_cells(const struct dt_node *node)
{
	if (!node->parent)
		return 0;
	return dt_prop_get_u32_def(node->parent, "#size-cells", 1);
}

u64 dt_get_address(const struct dt_node *node, unsigned int index,
		   u64 *out_size)
{
	const struct dt_property *p;
	u32 na = dt_n_address_cells(node);
	u32 ns = dt_n_size_cells(node);
	u32 pos, n;

	p = dt_require_property(node, "reg", -1);
	n = (na + ns) * sizeof(u32);
	pos = n * index;
	assert((pos + n) <= p->len);
	if (out_size)
		*out_size = dt_get_number(p->prop + pos + na * sizeof(u32), ns);
	return dt_get_number(p->prop + pos, na);
}

static u32 __dt_get_chip_id(const struct dt_node *node)
{
	const struct dt_property *prop;

	for (; node; node = node->parent) {
		prop = dt_find_property(node, "ibm,chip-id");
		if (prop)
			return dt_property_get_cell(prop, 0);
	}
	return 0xffffffff;
}

u32 dt_get_chip_id(const struct dt_node *node)
{
	u32 id = __dt_get_chip_id(node);
	assert(id != 0xffffffff);
	return id;
}

struct dt_node *dt_find_compatible_node_on_chip(struct dt_node *root,
						struct dt_node *prev,
						const char *compat,
						uint32_t chip_id)
{
	struct dt_node *node;

	node = prev ? dt_next(root, prev) : root;
	for (; node; node = dt_next(root, node)) {
		u32 cid = __dt_get_chip_id(node);
		if (cid == chip_id &&
		    dt_node_is_compatible(node, compat))
			return node;
	}
	return NULL;
}

unsigned int dt_count_addresses(const struct dt_node *node)
{
	const struct dt_property *p;
	u32 na = dt_n_address_cells(node);
	u32 ns = dt_n_size_cells(node);
	u32 n;

	p = dt_require_property(node, "reg", -1);
	n = (na + ns) * sizeof(u32);

	if (n == 0)
		return 0;

	return p->len / n;
}

u64 dt_translate_address(const struct dt_node *node, unsigned int index,
			 u64 *out_size)
{
	/* XXX TODO */
	return dt_get_address(node, index, out_size);
}

bool dt_node_is_enabled(struct dt_node *node)
{
	const struct dt_property *p = dt_find_property(node, "status");

	if (!p)
		return true;

	return p->len > 1 && p->prop[0] == 'o' && p->prop[1] == 'k';
}
