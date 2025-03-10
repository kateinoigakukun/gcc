/* __builtin_object_size (ptr, object_size_type) computation
   Copyright (C) 2004-2022 Free Software Foundation, Inc.
   Contributed by Jakub Jelinek <jakub@redhat.com>

This file is part of GCC.

GCC is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 3, or (at your option)
any later version.

GCC is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with GCC; see the file COPYING3.  If not see
<http://www.gnu.org/licenses/>.  */

#include "config.h"
#include "system.h"
#include "coretypes.h"
#include "backend.h"
#include "tree.h"
#include "gimple.h"
#include "tree-pass.h"
#include "ssa.h"
#include "gimple-pretty-print.h"
#include "fold-const.h"
#include "tree-object-size.h"
#include "gimple-fold.h"
#include "gimple-iterator.h"
#include "tree-cfg.h"
#include "stringpool.h"
#include "attribs.h"
#include "builtins.h"

struct object_size_info
{
  int object_size_type;
  unsigned char pass;
  bool changed;
  bitmap visited, reexamine;
  unsigned int *depths;
  unsigned int *stack, *tos;
};

struct GTY(()) object_size
{
  /* Estimate of bytes till the end of the object.  */
  tree size;
  /* Estimate of the size of the whole object.  */
  tree wholesize;
};

static tree compute_object_offset (const_tree, const_tree);
static bool addr_object_size (struct object_size_info *,
			      const_tree, int, tree *, tree *t = NULL);
static tree alloc_object_size (const gcall *, int);
static tree pass_through_call (const gcall *);
static void collect_object_sizes_for (struct object_size_info *, tree);
static void expr_object_size (struct object_size_info *, tree, tree);
static bool merge_object_sizes (struct object_size_info *, tree, tree);
static bool plus_stmt_object_size (struct object_size_info *, tree, gimple *);
static bool cond_expr_object_size (struct object_size_info *, tree, gimple *);
static void init_offset_limit (void);
static void check_for_plus_in_loops (struct object_size_info *, tree);
static void check_for_plus_in_loops_1 (struct object_size_info *, tree,
				       unsigned int);

/* object_sizes[0] is upper bound for the object size and number of bytes till
   the end of the object.
   object_sizes[1] is upper bound for the object size and number of bytes till
   the end of the subobject (innermost array or field with address taken).
   object_sizes[2] is lower bound for the object size and number of bytes till
   the end of the object and object_sizes[3] lower bound for subobject.  */
static vec<object_size> object_sizes[OST_END];

/* Bitmaps what object sizes have been computed already.  */
static bitmap computed[OST_END];

/* Maximum value of offset we consider to be addition.  */
static unsigned HOST_WIDE_INT offset_limit;

/* Return true if VAL is represents an unknown size for OBJECT_SIZE_TYPE.  */

static inline bool
size_unknown_p (tree val, int object_size_type)
{
  return ((object_size_type & OST_MINIMUM)
	  ? integer_zerop (val) : integer_all_onesp (val));
}

/* Return a tree with initial value for OBJECT_SIZE_TYPE.  */

static inline tree
size_initval (int object_size_type)
{
  return ((object_size_type & OST_MINIMUM)
	  ? TYPE_MAX_VALUE (sizetype) : size_zero_node);
}

/* Return a tree with unknown value for OBJECT_SIZE_TYPE.  */

static inline tree
size_unknown (int object_size_type)
{
  return ((object_size_type & OST_MINIMUM)
	  ? size_zero_node : TYPE_MAX_VALUE (sizetype));
}

/* Grow object_sizes[OBJECT_SIZE_TYPE] to num_ssa_names.  */

static inline void
object_sizes_grow (int object_size_type)
{
  if (num_ssa_names > object_sizes[object_size_type].length ())
    object_sizes[object_size_type].safe_grow (num_ssa_names, true);
}

/* Release object_sizes[OBJECT_SIZE_TYPE].  */

static inline void
object_sizes_release (int object_size_type)
{
  object_sizes[object_size_type].release ();
}

/* Return true if object_sizes[OBJECT_SIZE_TYPE][VARNO] is unknown.  */

static inline bool
object_sizes_unknown_p (int object_size_type, unsigned varno)
{
  return size_unknown_p (object_sizes[object_size_type][varno].size,
			 object_size_type);
}

/* Return size for VARNO corresponding to OSI.  If WHOLE is true, return the
   whole object size.  */

static inline tree
object_sizes_get (struct object_size_info *osi, unsigned varno,
		  bool whole = false)
{
  if (whole)
    return object_sizes[osi->object_size_type][varno].wholesize;
  else
    return object_sizes[osi->object_size_type][varno].size;
}

/* Set size for VARNO corresponding to OSI to VAL.  */

static inline void
object_sizes_initialize (struct object_size_info *osi, unsigned varno,
			 tree val, tree wholeval)
{
  int object_size_type = osi->object_size_type;

  object_sizes[object_size_type][varno].size = val;
  object_sizes[object_size_type][varno].wholesize = wholeval;
}

/* Set size for VARNO corresponding to OSI to VAL if it is the new minimum or
   maximum.  */

static inline bool
object_sizes_set (struct object_size_info *osi, unsigned varno, tree val,
		  tree wholeval)
{
  int object_size_type = osi->object_size_type;
  object_size osize = object_sizes[object_size_type][varno];

  tree oldval = osize.size;
  tree old_wholeval = osize.wholesize;

  enum tree_code code = object_size_type & OST_MINIMUM ? MIN_EXPR : MAX_EXPR;

  val = size_binop (code, val, oldval);
  wholeval = size_binop (code, wholeval, old_wholeval);

  object_sizes[object_size_type][varno].size = val;
  object_sizes[object_size_type][varno].wholesize = wholeval;
  return (tree_int_cst_compare (oldval, val) != 0
	  || tree_int_cst_compare (old_wholeval, wholeval) != 0);
}

/* Initialize OFFSET_LIMIT variable.  */
static void
init_offset_limit (void)
{
  if (tree_fits_uhwi_p (TYPE_MAX_VALUE (sizetype)))
    offset_limit = tree_to_uhwi (TYPE_MAX_VALUE (sizetype));
  else
    offset_limit = -1;
  offset_limit /= 2;
}

/* Bytes at end of the object with SZ from offset OFFSET.  If WHOLESIZE is not
   NULL_TREE, use it to get the net offset of the pointer, which should always
   be positive and hence, be within OFFSET_LIMIT for valid offsets.  */

static tree
size_for_offset (tree sz, tree offset, tree wholesize = NULL_TREE)
{
  gcc_checking_assert (TREE_CODE (offset) == INTEGER_CST);
  gcc_checking_assert (TREE_CODE (sz) == INTEGER_CST);
  gcc_checking_assert (types_compatible_p (TREE_TYPE (sz), sizetype));

  /* For negative offsets, if we have a distinct WHOLESIZE, use it to get a net
     offset from the whole object.  */
  if (wholesize && tree_int_cst_compare (sz, wholesize))
    {
      gcc_checking_assert (TREE_CODE (wholesize) == INTEGER_CST);
      gcc_checking_assert (types_compatible_p (TREE_TYPE (wholesize),
					       sizetype));

      /* Restructure SZ - OFFSET as
	 WHOLESIZE - (WHOLESIZE + OFFSET - SZ) so that the offset part, i.e.
	 WHOLESIZE + OFFSET - SZ is only allowed to be positive.  */
      tree tmp = size_binop (MAX_EXPR, wholesize, sz);
      offset = fold_build2 (PLUS_EXPR, sizetype, tmp, offset);
      offset = fold_build2 (MINUS_EXPR, sizetype, offset, sz);
      sz = tmp;
    }

  /* Safe to convert now, since a valid net offset should be non-negative.  */
  if (!types_compatible_p (TREE_TYPE (offset), sizetype))
    fold_convert (sizetype, offset);

  if (integer_zerop (offset))
    return sz;

  /* Negative or too large offset even after adjustment, cannot be within
     bounds of an object.  */
  if (compare_tree_int (offset, offset_limit) > 0)
    return size_zero_node;

  return size_binop (MINUS_EXPR, size_binop (MAX_EXPR, sz, offset), offset);
}

/* Compute offset of EXPR within VAR.  Return error_mark_node
   if unknown.  */

static tree
compute_object_offset (const_tree expr, const_tree var)
{
  enum tree_code code = PLUS_EXPR;
  tree base, off, t;

  if (expr == var)
    return size_zero_node;

  switch (TREE_CODE (expr))
    {
    case COMPONENT_REF:
      base = compute_object_offset (TREE_OPERAND (expr, 0), var);
      if (base == error_mark_node)
	return base;

      t = TREE_OPERAND (expr, 1);
      off = size_binop (PLUS_EXPR, DECL_FIELD_OFFSET (t),
			size_int (tree_to_uhwi (DECL_FIELD_BIT_OFFSET (t))
				  / BITS_PER_UNIT));
      break;

    case REALPART_EXPR:
    CASE_CONVERT:
    case VIEW_CONVERT_EXPR:
    case NON_LVALUE_EXPR:
      return compute_object_offset (TREE_OPERAND (expr, 0), var);

    case IMAGPART_EXPR:
      base = compute_object_offset (TREE_OPERAND (expr, 0), var);
      if (base == error_mark_node)
	return base;

      off = TYPE_SIZE_UNIT (TREE_TYPE (expr));
      break;

    case ARRAY_REF:
      base = compute_object_offset (TREE_OPERAND (expr, 0), var);
      if (base == error_mark_node)
	return base;

      t = TREE_OPERAND (expr, 1);
      tree low_bound, unit_size;
      low_bound = array_ref_low_bound (CONST_CAST_TREE (expr));
      unit_size = array_ref_element_size (CONST_CAST_TREE (expr));
      if (! integer_zerop (low_bound))
	t = fold_build2 (MINUS_EXPR, TREE_TYPE (t), t, low_bound);
      if (TREE_CODE (t) == INTEGER_CST && tree_int_cst_sgn (t) < 0)
	{
	  code = MINUS_EXPR;
	  t = fold_build1 (NEGATE_EXPR, TREE_TYPE (t), t);
	}
      t = fold_convert (sizetype, t);
      off = size_binop (MULT_EXPR, unit_size, t);
      break;

    case MEM_REF:
      gcc_assert (TREE_CODE (TREE_OPERAND (expr, 0)) == ADDR_EXPR);
      return wide_int_to_tree (sizetype, mem_ref_offset (expr));

    default:
      return error_mark_node;
    }

  return size_binop (code, base, off);
}

/* Returns the size of the object designated by DECL considering its
   initializer if it either has one or if it would not affect its size,
   otherwise the size of the object without the initializer when MIN
   is true, else null.  An object's initializer affects the object's
   size if it's a struct type with a flexible array member.  */

tree
decl_init_size (tree decl, bool min)
{
  tree size = DECL_SIZE_UNIT (decl);
  tree type = TREE_TYPE (decl);
  if (TREE_CODE (type) != RECORD_TYPE)
    return size;

  tree last = last_field (type);
  if (!last)
    return size;

  tree last_type = TREE_TYPE (last);
  if (TREE_CODE (last_type) != ARRAY_TYPE
      || TYPE_SIZE (last_type))
    return size;

  /* Use TYPE_SIZE_UNIT; DECL_SIZE_UNIT sometimes reflects the size
     of the initializer and sometimes doesn't.  */
  size = TYPE_SIZE_UNIT (type);
  tree ref = build3 (COMPONENT_REF, type, decl, last, NULL_TREE);
  tree compsize = component_ref_size (ref);
  if (!compsize)
    return min ? size : NULL_TREE;

  /* The size includes tail padding and initializer elements.  */
  tree pos = byte_position (last);
  size = fold_build2 (PLUS_EXPR, TREE_TYPE (size), pos, compsize);
  return size;
}

/* Compute __builtin_object_size for PTR, which is a ADDR_EXPR.
   OBJECT_SIZE_TYPE is the second argument from __builtin_object_size.
   If unknown, return size_unknown (object_size_type).  */

static bool
addr_object_size (struct object_size_info *osi, const_tree ptr,
		  int object_size_type, tree *psize, tree *pwholesize)
{
  tree pt_var, pt_var_size = NULL_TREE, pt_var_wholesize = NULL_TREE;
  tree var_size, bytes, wholebytes;

  gcc_assert (TREE_CODE (ptr) == ADDR_EXPR);

  /* Set to unknown and overwrite just before returning if the size
     could be determined.  */
  *psize = size_unknown (object_size_type);
  if (pwholesize)
    *pwholesize = size_unknown (object_size_type);

  pt_var = TREE_OPERAND (ptr, 0);
  while (handled_component_p (pt_var))
    pt_var = TREE_OPERAND (pt_var, 0);

  if (!pt_var)
    return false;

  if (TREE_CODE (pt_var) == MEM_REF)
    {
      tree sz, wholesize;

      if (!osi || (object_size_type & OST_SUBOBJECT) != 0
	  || TREE_CODE (TREE_OPERAND (pt_var, 0)) != SSA_NAME)
	{
	  compute_builtin_object_size (TREE_OPERAND (pt_var, 0),
				       object_size_type & ~OST_SUBOBJECT, &sz);
	  wholesize = sz;
	}
      else
	{
	  tree var = TREE_OPERAND (pt_var, 0);
	  if (osi->pass == 0)
	    collect_object_sizes_for (osi, var);
	  if (bitmap_bit_p (computed[object_size_type],
			    SSA_NAME_VERSION (var)))
	    {
	      sz = object_sizes_get (osi, SSA_NAME_VERSION (var));
	      wholesize = object_sizes_get (osi, SSA_NAME_VERSION (var), true);
	    }
	  else
	    sz = wholesize = size_unknown (object_size_type);
	}
      if (!size_unknown_p (sz, object_size_type))
	{
	  tree offset = TREE_OPERAND (pt_var, 1);
	  if (TREE_CODE (offset) != INTEGER_CST
	      || TREE_CODE (sz) != INTEGER_CST)
	    sz = wholesize = size_unknown (object_size_type);
	  else
	    sz = size_for_offset (sz, offset, wholesize);
	}

      if (!size_unknown_p (sz, object_size_type)
	  && TREE_CODE (sz) == INTEGER_CST
	  && compare_tree_int (sz, offset_limit) < 0)
	{
	  pt_var_size = sz;
	  pt_var_wholesize = wholesize;
	}
    }
  else if (DECL_P (pt_var))
    {
      pt_var_size = pt_var_wholesize
	= decl_init_size (pt_var, object_size_type & OST_MINIMUM);
      if (!pt_var_size)
	return false;
    }
  else if (TREE_CODE (pt_var) == STRING_CST)
    pt_var_size = pt_var_wholesize = TYPE_SIZE_UNIT (TREE_TYPE (pt_var));
  else
    return false;

  if (pt_var_size)
    {
      /* Validate the size determined above.  */
      if (compare_tree_int (pt_var_size, offset_limit) >= 0)
	return false;
    }

  if (pt_var != TREE_OPERAND (ptr, 0))
    {
      tree var;

      if (object_size_type & OST_SUBOBJECT)
	{
	  var = TREE_OPERAND (ptr, 0);

	  while (var != pt_var
		 && TREE_CODE (var) != BIT_FIELD_REF
		 && TREE_CODE (var) != COMPONENT_REF
		 && TREE_CODE (var) != ARRAY_REF
		 && TREE_CODE (var) != ARRAY_RANGE_REF
		 && TREE_CODE (var) != REALPART_EXPR
		 && TREE_CODE (var) != IMAGPART_EXPR)
	    var = TREE_OPERAND (var, 0);
	  if (var != pt_var && TREE_CODE (var) == ARRAY_REF)
	    var = TREE_OPERAND (var, 0);
	  if (! TYPE_SIZE_UNIT (TREE_TYPE (var))
	      || ! tree_fits_uhwi_p (TYPE_SIZE_UNIT (TREE_TYPE (var)))
	      || (pt_var_size
		  && tree_int_cst_lt (pt_var_size,
				      TYPE_SIZE_UNIT (TREE_TYPE (var)))))
	    var = pt_var;
	  else if (var != pt_var && TREE_CODE (pt_var) == MEM_REF)
	    {
	      tree v = var;
	      /* For &X->fld, compute object size only if fld isn't the last
		 field, as struct { int i; char c[1]; } is often used instead
		 of flexible array member.  */
	      while (v && v != pt_var)
		switch (TREE_CODE (v))
		  {
		  case ARRAY_REF:
		    if (TYPE_SIZE_UNIT (TREE_TYPE (TREE_OPERAND (v, 0)))
			&& TREE_CODE (TREE_OPERAND (v, 1)) == INTEGER_CST)
		      {
			tree domain
			  = TYPE_DOMAIN (TREE_TYPE (TREE_OPERAND (v, 0)));
			if (domain
			    && TYPE_MAX_VALUE (domain)
			    && TREE_CODE (TYPE_MAX_VALUE (domain))
			       == INTEGER_CST
			    && tree_int_cst_lt (TREE_OPERAND (v, 1),
						TYPE_MAX_VALUE (domain)))
			  {
			    v = NULL_TREE;
			    break;
			  }
		      }
		    v = TREE_OPERAND (v, 0);
		    break;
		  case REALPART_EXPR:
		  case IMAGPART_EXPR:
		    v = NULL_TREE;
		    break;
		  case COMPONENT_REF:
		    if (TREE_CODE (TREE_TYPE (v)) != ARRAY_TYPE)
		      {
			v = NULL_TREE;
			break;
		      }
		    while (v != pt_var && TREE_CODE (v) == COMPONENT_REF)
		      if (TREE_CODE (TREE_TYPE (TREE_OPERAND (v, 0)))
			  != UNION_TYPE
			  && TREE_CODE (TREE_TYPE (TREE_OPERAND (v, 0)))
			  != QUAL_UNION_TYPE)
			break;
		      else
			v = TREE_OPERAND (v, 0);
		    if (TREE_CODE (v) == COMPONENT_REF
			&& TREE_CODE (TREE_TYPE (TREE_OPERAND (v, 0)))
			   == RECORD_TYPE)
		      {
			tree fld_chain = DECL_CHAIN (TREE_OPERAND (v, 1));
			for (; fld_chain; fld_chain = DECL_CHAIN (fld_chain))
			  if (TREE_CODE (fld_chain) == FIELD_DECL)
			    break;

			if (fld_chain)
			  {
			    v = NULL_TREE;
			    break;
			  }
			v = TREE_OPERAND (v, 0);
		      }
		    while (v != pt_var && TREE_CODE (v) == COMPONENT_REF)
		      if (TREE_CODE (TREE_TYPE (TREE_OPERAND (v, 0)))
			  != UNION_TYPE
			  && TREE_CODE (TREE_TYPE (TREE_OPERAND (v, 0)))
			  != QUAL_UNION_TYPE)
			break;
		      else
			v = TREE_OPERAND (v, 0);
		    if (v != pt_var)
		      v = NULL_TREE;
		    else
		      v = pt_var;
		    break;
		  default:
		    v = pt_var;
		    break;
		  }
	      if (v == pt_var)
		var = pt_var;
	    }
	}
      else
	var = pt_var;

      if (var != pt_var)
	var_size = TYPE_SIZE_UNIT (TREE_TYPE (var));
      else if (!pt_var_size)
	return false;
      else
	var_size = pt_var_size;
      bytes = compute_object_offset (TREE_OPERAND (ptr, 0), var);
      if (bytes != error_mark_node)
	{
	  if (TREE_CODE (bytes) == INTEGER_CST
	      && tree_int_cst_lt (var_size, bytes))
	    bytes = size_zero_node;
	  else
	    bytes = size_binop (MINUS_EXPR, var_size, bytes);
	}
      if (var != pt_var
	  && pt_var_size
	  && TREE_CODE (pt_var) == MEM_REF
	  && bytes != error_mark_node)
	{
	  tree bytes2 = compute_object_offset (TREE_OPERAND (ptr, 0), pt_var);
	  if (bytes2 != error_mark_node)
	    {
	      if (TREE_CODE (bytes2) == INTEGER_CST
		  && tree_int_cst_lt (pt_var_size, bytes2))
		bytes2 = size_zero_node;
	      else
		bytes2 = size_binop (MINUS_EXPR, pt_var_size, bytes2);
	      bytes = size_binop (MIN_EXPR, bytes, bytes2);
	    }
	}

      wholebytes
	= object_size_type & OST_SUBOBJECT ? var_size : pt_var_wholesize;
    }
  else if (!pt_var_size)
    return false;
  else
    {
      bytes = pt_var_size;
      wholebytes = pt_var_wholesize;
    }

  if (TREE_CODE (bytes) != INTEGER_CST
      || TREE_CODE (wholebytes) != INTEGER_CST)
    return false;

  *psize = bytes;
  if (pwholesize)
    *pwholesize = wholebytes;
  return true;
}


/* Compute __builtin_object_size for CALL, which is a GIMPLE_CALL.
   Handles calls to functions declared with attribute alloc_size.
   OBJECT_SIZE_TYPE is the second argument from __builtin_object_size.
   If unknown, return size_unknown (object_size_type).  */

static tree
alloc_object_size (const gcall *call, int object_size_type)
{
  gcc_assert (is_gimple_call (call));

  tree calltype;
  if (tree callfn = gimple_call_fndecl (call))
    calltype = TREE_TYPE (callfn);
  else
    calltype = gimple_call_fntype (call);

  if (!calltype)
    return size_unknown (object_size_type);

  /* Set to positions of alloc_size arguments.  */
  int arg1 = -1, arg2 = -1;
  tree alloc_size = lookup_attribute ("alloc_size",
				      TYPE_ATTRIBUTES (calltype));
  if (alloc_size && TREE_VALUE (alloc_size))
    {
      tree p = TREE_VALUE (alloc_size);

      arg1 = TREE_INT_CST_LOW (TREE_VALUE (p))-1;
      if (TREE_CHAIN (p))
        arg2 = TREE_INT_CST_LOW (TREE_VALUE (TREE_CHAIN (p)))-1;
    }

  if (arg1 < 0 || arg1 >= (int)gimple_call_num_args (call)
      || TREE_CODE (gimple_call_arg (call, arg1)) != INTEGER_CST
      || (arg2 >= 0
	  && (arg2 >= (int)gimple_call_num_args (call)
	      || TREE_CODE (gimple_call_arg (call, arg2)) != INTEGER_CST)))
    return size_unknown (object_size_type);

  tree bytes = NULL_TREE;
  if (arg2 >= 0)
    bytes = size_binop (MULT_EXPR,
	fold_convert (sizetype, gimple_call_arg (call, arg1)),
	fold_convert (sizetype, gimple_call_arg (call, arg2)));
  else if (arg1 >= 0)
    bytes = fold_convert (sizetype, gimple_call_arg (call, arg1));

  return bytes;
}


/* If object size is propagated from one of function's arguments directly
   to its return value, return that argument for GIMPLE_CALL statement CALL.
   Otherwise return NULL.  */

static tree
pass_through_call (const gcall *call)
{
  unsigned rf = gimple_call_return_flags (call);
  if (rf & ERF_RETURNS_ARG)
    {
      unsigned argnum = rf & ERF_RETURN_ARG_MASK;
      if (argnum < gimple_call_num_args (call))
	return gimple_call_arg (call, argnum);
    }

  /* __builtin_assume_aligned is intentionally not marked RET1.  */
  if (gimple_call_builtin_p (call, BUILT_IN_ASSUME_ALIGNED))
    return gimple_call_arg (call, 0);

  return NULL_TREE;
}


/* Compute __builtin_object_size value for PTR and set *PSIZE to
   the resulting value.  If the declared object is known and PDECL
   is nonnull, sets *PDECL to the object's DECL.  OBJECT_SIZE_TYPE
   is the second argument   to __builtin_object_size.
   Returns true on success and false when the object size could not
   be determined.  */

bool
compute_builtin_object_size (tree ptr, int object_size_type,
			     tree *psize)
{
  gcc_assert (object_size_type >= 0 && object_size_type < OST_END);

  /* Set to unknown and overwrite just before returning if the size
     could be determined.  */
  *psize = size_unknown (object_size_type);

  if (! offset_limit)
    init_offset_limit ();

  if (TREE_CODE (ptr) == ADDR_EXPR)
    return addr_object_size (NULL, ptr, object_size_type, psize);

  if (TREE_CODE (ptr) != SSA_NAME
      || !POINTER_TYPE_P (TREE_TYPE (ptr)))
      return false;

  if (computed[object_size_type] == NULL)
    {
      if (optimize || object_size_type & OST_SUBOBJECT)
	return false;

      /* When not optimizing, rather than failing, make a small effort
	 to determine the object size without the full benefit of
	 the (costly) computation below.  */
      gimple *def = SSA_NAME_DEF_STMT (ptr);
      if (gimple_code (def) == GIMPLE_ASSIGN)
	{
	  tree_code code = gimple_assign_rhs_code (def);
	  if (code == POINTER_PLUS_EXPR)
	    {
	      tree offset = gimple_assign_rhs2 (def);
	      ptr = gimple_assign_rhs1 (def);

	      if (tree_fits_shwi_p (offset)
		  && compute_builtin_object_size (ptr, object_size_type,
						  psize))
		{
		  /* Return zero when the offset is out of bounds.  */
		  *psize = size_for_offset (*psize, offset);
		  return true;
		}
	    }
	}
      return false;
    }

  struct object_size_info osi;
  osi.object_size_type = object_size_type;
  if (!bitmap_bit_p (computed[object_size_type], SSA_NAME_VERSION (ptr)))
    {
      bitmap_iterator bi;
      unsigned int i;

      object_sizes_grow (object_size_type);
      if (dump_file)
	{
	  fprintf (dump_file, "Computing %s %s%sobject size for ",
		   (object_size_type & OST_MINIMUM) ? "minimum" : "maximum",
		   (object_size_type & OST_DYNAMIC) ? "dynamic " : "",
		   (object_size_type & OST_SUBOBJECT) ? "sub" : "");
	  print_generic_expr (dump_file, ptr, dump_flags);
	  fprintf (dump_file, ":\n");
	}

      osi.visited = BITMAP_ALLOC (NULL);
      osi.reexamine = BITMAP_ALLOC (NULL);
      osi.depths = NULL;
      osi.stack = NULL;
      osi.tos = NULL;

      /* First pass: walk UD chains, compute object sizes that
	 can be computed.  osi.reexamine bitmap at the end will
	 contain what variables were found in dependency cycles
	 and therefore need to be reexamined.  */
      osi.pass = 0;
      osi.changed = false;
      collect_object_sizes_for (&osi, ptr);

      /* Second pass: keep recomputing object sizes of variables
	 that need reexamination, until no object sizes are
	 increased or all object sizes are computed.  */
      if (! bitmap_empty_p (osi.reexamine))
	{
	  bitmap reexamine = BITMAP_ALLOC (NULL);

	  /* If looking for minimum instead of maximum object size,
	     detect cases where a pointer is increased in a loop.
	     Although even without this detection pass 2 would eventually
	     terminate, it could take a long time.  If a pointer is
	     increasing this way, we need to assume 0 object size.
	     E.g. p = &buf[0]; while (cond) p = p + 4;  */
	  if (object_size_type & OST_MINIMUM)
	    {
	      osi.depths = XCNEWVEC (unsigned int, num_ssa_names);
	      osi.stack = XNEWVEC (unsigned int, num_ssa_names);
	      osi.tos = osi.stack;
	      osi.pass = 1;
	      /* collect_object_sizes_for is changing
		 osi.reexamine bitmap, so iterate over a copy.  */
	      bitmap_copy (reexamine, osi.reexamine);
	      EXECUTE_IF_SET_IN_BITMAP (reexamine, 0, i, bi)
		if (bitmap_bit_p (osi.reexamine, i))
		  check_for_plus_in_loops (&osi, ssa_name (i));

	      free (osi.depths);
	      osi.depths = NULL;
	      free (osi.stack);
	      osi.stack = NULL;
	      osi.tos = NULL;
	    }

	  do
	    {
	      osi.pass = 2;
	      osi.changed = false;
	      /* collect_object_sizes_for is changing
		 osi.reexamine bitmap, so iterate over a copy.  */
	      bitmap_copy (reexamine, osi.reexamine);
	      EXECUTE_IF_SET_IN_BITMAP (reexamine, 0, i, bi)
		if (bitmap_bit_p (osi.reexamine, i))
		  {
		    collect_object_sizes_for (&osi, ssa_name (i));
		    if (dump_file && (dump_flags & TDF_DETAILS))
		      {
			fprintf (dump_file, "Reexamining ");
			print_generic_expr (dump_file, ssa_name (i),
					    dump_flags);
			fprintf (dump_file, "\n");
		      }
		  }
	    }
	  while (osi.changed);

	  BITMAP_FREE (reexamine);
	}
      EXECUTE_IF_SET_IN_BITMAP (osi.reexamine, 0, i, bi)
	bitmap_set_bit (computed[object_size_type], i);

      /* Debugging dumps.  */
      if (dump_file)
	{
	  EXECUTE_IF_SET_IN_BITMAP (osi.visited, 0, i, bi)
	    if (!object_sizes_unknown_p (object_size_type, i))
	      {
		print_generic_expr (dump_file, ssa_name (i),
				    dump_flags);
		fprintf (dump_file,
			 ": %s %s%sobject size ",
			 ((object_size_type & OST_MINIMUM) ? "minimum"
			  : "maximum"),
			 (object_size_type & OST_DYNAMIC) ? "dynamic " : "",
			 (object_size_type & OST_SUBOBJECT) ? "sub" : "");
		print_generic_expr (dump_file, object_sizes_get (&osi, i),
				    dump_flags);
		fprintf (dump_file, "\n");
	      }
	}

      BITMAP_FREE (osi.reexamine);
      BITMAP_FREE (osi.visited);
    }

  *psize = object_sizes_get (&osi, SSA_NAME_VERSION (ptr));
  return !size_unknown_p (*psize, object_size_type);
}

/* Compute object_sizes for PTR, defined to VALUE, which is not an SSA_NAME.  */

static void
expr_object_size (struct object_size_info *osi, tree ptr, tree value)
{
  int object_size_type = osi->object_size_type;
  unsigned int varno = SSA_NAME_VERSION (ptr);
  tree bytes, wholesize;

  gcc_assert (!object_sizes_unknown_p (object_size_type, varno));
  gcc_assert (osi->pass == 0);

  if (TREE_CODE (value) == WITH_SIZE_EXPR)
    value = TREE_OPERAND (value, 0);

  /* Pointer variables should have been handled by merge_object_sizes.  */
  gcc_assert (TREE_CODE (value) != SSA_NAME
	      || !POINTER_TYPE_P (TREE_TYPE (value)));

  if (TREE_CODE (value) == ADDR_EXPR)
    addr_object_size (osi, value, object_size_type, &bytes, &wholesize);
  else
    bytes = wholesize = size_unknown (object_size_type);

  object_sizes_set (osi, varno, bytes, wholesize);
}


/* Compute object_sizes for PTR, defined to the result of a call.  */

static void
call_object_size (struct object_size_info *osi, tree ptr, gcall *call)
{
  int object_size_type = osi->object_size_type;
  unsigned int varno = SSA_NAME_VERSION (ptr);

  gcc_assert (is_gimple_call (call));

  gcc_assert (!object_sizes_unknown_p (object_size_type, varno));
  gcc_assert (osi->pass == 0);
  tree bytes = alloc_object_size (call, object_size_type);

  object_sizes_set (osi, varno, bytes, bytes);
}


/* Compute object_sizes for PTR, defined to an unknown value.  */

static void
unknown_object_size (struct object_size_info *osi, tree ptr)
{
  int object_size_type = osi->object_size_type;
  unsigned int varno = SSA_NAME_VERSION (ptr);

  gcc_checking_assert (!object_sizes_unknown_p (object_size_type, varno));
  gcc_checking_assert (osi->pass == 0);
  tree bytes = size_unknown (object_size_type);

  object_sizes_set (osi, varno, bytes, bytes);
}


/* Merge object sizes of ORIG + OFFSET into DEST.  Return true if
   the object size might need reexamination later.  */

static bool
merge_object_sizes (struct object_size_info *osi, tree dest, tree orig)
{
  int object_size_type = osi->object_size_type;
  unsigned int varno = SSA_NAME_VERSION (dest);
  tree orig_bytes, wholesize;

  if (object_sizes_unknown_p (object_size_type, varno))
    return false;

  if (osi->pass == 0)
    collect_object_sizes_for (osi, orig);

  orig_bytes = object_sizes_get (osi, SSA_NAME_VERSION (orig));
  wholesize = object_sizes_get (osi, SSA_NAME_VERSION (orig), true);

  if (object_sizes_set (osi, varno, orig_bytes, wholesize))
    osi->changed = true;

  return bitmap_bit_p (osi->reexamine, SSA_NAME_VERSION (orig));
}


/* Compute object_sizes for VAR, defined to the result of an assignment
   with operator POINTER_PLUS_EXPR.  Return true if the object size might
   need reexamination  later.  */

static bool
plus_stmt_object_size (struct object_size_info *osi, tree var, gimple *stmt)
{
  int object_size_type = osi->object_size_type;
  unsigned int varno = SSA_NAME_VERSION (var);
  tree bytes, wholesize;
  tree op0, op1;
  bool reexamine = false;

  if (gimple_assign_rhs_code (stmt) == POINTER_PLUS_EXPR)
    {
      op0 = gimple_assign_rhs1 (stmt);
      op1 = gimple_assign_rhs2 (stmt);
    }
  else if (gimple_assign_rhs_code (stmt) == ADDR_EXPR)
    {
      tree rhs = TREE_OPERAND (gimple_assign_rhs1 (stmt), 0);
      gcc_assert (TREE_CODE (rhs) == MEM_REF);
      op0 = TREE_OPERAND (rhs, 0);
      op1 = TREE_OPERAND (rhs, 1);
    }
  else
    gcc_unreachable ();

  if (object_sizes_unknown_p (object_size_type, varno))
    return false;

  /* Handle PTR + OFFSET here.  */
  if (TREE_CODE (op1) == INTEGER_CST
      && (TREE_CODE (op0) == SSA_NAME
	  || TREE_CODE (op0) == ADDR_EXPR))
    {
      if (TREE_CODE (op0) == SSA_NAME)
	{
	  if (osi->pass == 0)
	    collect_object_sizes_for (osi, op0);

	  bytes = object_sizes_get (osi, SSA_NAME_VERSION (op0));
	  wholesize = object_sizes_get (osi, SSA_NAME_VERSION (op0), true);
	  reexamine = bitmap_bit_p (osi->reexamine, SSA_NAME_VERSION (op0));
	}
      else
	{
	  /* op0 will be ADDR_EXPR here.  We should never come here during
	     reexamination.  */
	  gcc_checking_assert (osi->pass == 0);
	  addr_object_size (osi, op0, object_size_type, &bytes, &wholesize);
	}

      /* In the first pass, do not compute size for offset if either the
	 maximum size is unknown or the minimum size is not initialized yet;
	 the latter indicates a dependency loop and will be resolved in
	 subsequent passes.  We attempt to compute offset for 0 minimum size
	 too because a negative offset could be within bounds of WHOLESIZE,
	 giving a non-zero result for VAR.  */
      if (osi->pass != 0 || !size_unknown_p (bytes, 0))
	bytes = size_for_offset (bytes, op1, wholesize);
    }
  else
    bytes = wholesize = size_unknown (object_size_type);

  if (object_sizes_set (osi, varno, bytes, wholesize))
    osi->changed = true;
  return reexamine;
}


/* Compute object_sizes for VAR, defined at STMT, which is
   a COND_EXPR.  Return true if the object size might need reexamination
   later.  */

static bool
cond_expr_object_size (struct object_size_info *osi, tree var, gimple *stmt)
{
  tree then_, else_;
  int object_size_type = osi->object_size_type;
  unsigned int varno = SSA_NAME_VERSION (var);
  bool reexamine = false;

  gcc_assert (gimple_assign_rhs_code (stmt) == COND_EXPR);

  if (object_sizes_unknown_p (object_size_type, varno))
    return false;

  then_ = gimple_assign_rhs2 (stmt);
  else_ = gimple_assign_rhs3 (stmt);

  if (TREE_CODE (then_) == SSA_NAME)
    reexamine |= merge_object_sizes (osi, var, then_);
  else
    expr_object_size (osi, var, then_);

  if (object_sizes_unknown_p (object_size_type, varno))
    return reexamine;

  if (TREE_CODE (else_) == SSA_NAME)
    reexamine |= merge_object_sizes (osi, var, else_);
  else
    expr_object_size (osi, var, else_);

  return reexamine;
}

/* Compute object sizes for VAR.
   For ADDR_EXPR an object size is the number of remaining bytes
   to the end of the object (where what is considered an object depends on
   OSI->object_size_type).
   For allocation GIMPLE_CALL like malloc or calloc object size is the size
   of the allocation.
   For POINTER_PLUS_EXPR where second operand is a constant integer,
   object size is object size of the first operand minus the constant.
   If the constant is bigger than the number of remaining bytes until the
   end of the object, object size is 0, but if it is instead a pointer
   subtraction, object size is size_unknown (object_size_type).
   To differentiate addition from subtraction, ADDR_EXPR returns
   size_unknown (object_size_type) for all objects bigger than half of the
   address space, and constants less than half of the address space are
   considered addition, while bigger constants subtraction.
   For a memcpy like GIMPLE_CALL that always returns one of its arguments, the
   object size is object size of that argument.
   Otherwise, object size is the maximum of object sizes of variables
   that it might be set to.  */

static void
collect_object_sizes_for (struct object_size_info *osi, tree var)
{
  int object_size_type = osi->object_size_type;
  unsigned int varno = SSA_NAME_VERSION (var);
  gimple *stmt;
  bool reexamine;

  if (bitmap_bit_p (computed[object_size_type], varno))
    return;

  if (osi->pass == 0)
    {
      if (bitmap_set_bit (osi->visited, varno))
	{
	  /* Initialize to 0 for maximum size and M1U for minimum size so that
	     it gets immediately overridden.  */
	  object_sizes_initialize (osi, varno,
				   size_initval (object_size_type),
				   size_initval (object_size_type));
	}
      else
	{
	  /* Found a dependency loop.  Mark the variable for later
	     re-examination.  */
	  bitmap_set_bit (osi->reexamine, varno);
	  if (dump_file && (dump_flags & TDF_DETAILS))
	    {
	      fprintf (dump_file, "Found a dependency loop at ");
	      print_generic_expr (dump_file, var, dump_flags);
	      fprintf (dump_file, "\n");
	    }
	  return;
	}
    }

  if (dump_file && (dump_flags & TDF_DETAILS))
    {
      fprintf (dump_file, "Visiting use-def links for ");
      print_generic_expr (dump_file, var, dump_flags);
      fprintf (dump_file, "\n");
    }

  stmt = SSA_NAME_DEF_STMT (var);
  reexamine = false;

  switch (gimple_code (stmt))
    {
    case GIMPLE_ASSIGN:
      {
	tree rhs = gimple_assign_rhs1 (stmt);
        if (gimple_assign_rhs_code (stmt) == POINTER_PLUS_EXPR
	    || (gimple_assign_rhs_code (stmt) == ADDR_EXPR
		&& TREE_CODE (TREE_OPERAND (rhs, 0)) == MEM_REF))
          reexamine = plus_stmt_object_size (osi, var, stmt);
	else if (gimple_assign_rhs_code (stmt) == COND_EXPR)
	  reexamine = cond_expr_object_size (osi, var, stmt);
        else if (gimple_assign_single_p (stmt)
                 || gimple_assign_unary_nop_p (stmt))
          {
            if (TREE_CODE (rhs) == SSA_NAME
                && POINTER_TYPE_P (TREE_TYPE (rhs)))
	      reexamine = merge_object_sizes (osi, var, rhs);
            else
              expr_object_size (osi, var, rhs);
          }
        else
	  unknown_object_size (osi, var);
        break;
      }

    case GIMPLE_CALL:
      {
	gcall *call_stmt = as_a <gcall *> (stmt);
        tree arg = pass_through_call (call_stmt);
        if (arg)
          {
            if (TREE_CODE (arg) == SSA_NAME
                && POINTER_TYPE_P (TREE_TYPE (arg)))
	      reexamine = merge_object_sizes (osi, var, arg);
            else
              expr_object_size (osi, var, arg);
          }
        else
          call_object_size (osi, var, call_stmt);
	break;
      }

    case GIMPLE_ASM:
      /* Pointers defined by __asm__ statements can point anywhere.  */
      unknown_object_size (osi, var);
      break;

    case GIMPLE_NOP:
      if (SSA_NAME_VAR (var)
	  && TREE_CODE (SSA_NAME_VAR (var)) == PARM_DECL)
	expr_object_size (osi, var, SSA_NAME_VAR (var));
      else
	/* Uninitialized SSA names point nowhere.  */
	unknown_object_size (osi, var);
      break;

    case GIMPLE_PHI:
      {
	unsigned i;

	for (i = 0; i < gimple_phi_num_args (stmt); i++)
	  {
	    tree rhs = gimple_phi_arg (stmt, i)->def;

	    if (object_sizes_unknown_p (object_size_type, varno))
	      break;

	    if (TREE_CODE (rhs) == SSA_NAME)
	      reexamine |= merge_object_sizes (osi, var, rhs);
	    else if (osi->pass == 0)
	      expr_object_size (osi, var, rhs);
	  }
	break;
      }

    default:
      gcc_unreachable ();
    }

  if (! reexamine || object_sizes_unknown_p (object_size_type, varno))
    {
      bitmap_set_bit (computed[object_size_type], varno);
      bitmap_clear_bit (osi->reexamine, varno);
    }
  else
    {
      bitmap_set_bit (osi->reexamine, varno);
      if (dump_file && (dump_flags & TDF_DETAILS))
	{
	  fprintf (dump_file, "Need to reexamine ");
	  print_generic_expr (dump_file, var, dump_flags);
	  fprintf (dump_file, "\n");
	}
    }
}


/* Helper function for check_for_plus_in_loops.  Called recursively
   to detect loops.  */

static void
check_for_plus_in_loops_1 (struct object_size_info *osi, tree var,
			   unsigned int depth)
{
  gimple *stmt = SSA_NAME_DEF_STMT (var);
  unsigned int varno = SSA_NAME_VERSION (var);

  if (osi->depths[varno])
    {
      if (osi->depths[varno] != depth)
	{
	  unsigned int *sp;

	  /* Found a loop involving pointer addition.  */
	  for (sp = osi->tos; sp > osi->stack; )
	    {
	      --sp;
	      bitmap_clear_bit (osi->reexamine, *sp);
	      bitmap_set_bit (computed[osi->object_size_type], *sp);
	      object_sizes_set (osi, *sp, size_zero_node,
				object_sizes_get (osi, *sp, true));
	      if (*sp == varno)
		break;
	    }
	}
      return;
    }
  else if (! bitmap_bit_p (osi->reexamine, varno))
    return;

  osi->depths[varno] = depth;
  *osi->tos++ = varno;

  switch (gimple_code (stmt))
    {

    case GIMPLE_ASSIGN:
      {
        if ((gimple_assign_single_p (stmt)
             || gimple_assign_unary_nop_p (stmt))
            && TREE_CODE (gimple_assign_rhs1 (stmt)) == SSA_NAME)
          {
            tree rhs = gimple_assign_rhs1 (stmt);

            check_for_plus_in_loops_1 (osi, rhs, depth);
          }
        else if (gimple_assign_rhs_code (stmt) == POINTER_PLUS_EXPR)
          {
            tree basevar = gimple_assign_rhs1 (stmt);
            tree cst = gimple_assign_rhs2 (stmt);

            gcc_assert (TREE_CODE (cst) == INTEGER_CST);

            check_for_plus_in_loops_1 (osi, basevar,
                                       depth + !integer_zerop (cst));
          }
        else
          gcc_unreachable ();
        break;
      }

    case GIMPLE_CALL:
      {
	gcall *call_stmt = as_a <gcall *> (stmt);
        tree arg = pass_through_call (call_stmt);
        if (arg)
          {
            if (TREE_CODE (arg) == SSA_NAME)
              check_for_plus_in_loops_1 (osi, arg, depth);
            else
              gcc_unreachable ();
          }
        break;
      }

    case GIMPLE_PHI:
      {
	unsigned i;

	for (i = 0; i < gimple_phi_num_args (stmt); i++)
	  {
	    tree rhs = gimple_phi_arg (stmt, i)->def;

	    if (TREE_CODE (rhs) == SSA_NAME)
	      check_for_plus_in_loops_1 (osi, rhs, depth);
	  }
	break;
      }

    default:
      gcc_unreachable ();
    }

  osi->depths[varno] = 0;
  osi->tos--;
}


/* Check if some pointer we are computing object size of is being increased
   within a loop.  If yes, assume all the SSA variables participating in
   that loop have minimum object sizes 0.  */

static void
check_for_plus_in_loops (struct object_size_info *osi, tree var)
{
  gimple *stmt = SSA_NAME_DEF_STMT (var);

  /* NOTE: In the pre-tuples code, we handled a CALL_EXPR here,
     and looked for a POINTER_PLUS_EXPR in the pass-through
     argument, if any.  In GIMPLE, however, such an expression
     is not a valid call operand.  */

  if (is_gimple_assign (stmt)
      && gimple_assign_rhs_code (stmt) == POINTER_PLUS_EXPR)
    {
      tree basevar = gimple_assign_rhs1 (stmt);
      tree cst = gimple_assign_rhs2 (stmt);

      gcc_assert (TREE_CODE (cst) == INTEGER_CST);

      /* Skip non-positive offsets.  */
      if (integer_zerop (cst) || compare_tree_int (cst, offset_limit) > 0)
        return;

      osi->depths[SSA_NAME_VERSION (basevar)] = 1;
      *osi->tos++ = SSA_NAME_VERSION (basevar);
      check_for_plus_in_loops_1 (osi, var, 2);
      osi->depths[SSA_NAME_VERSION (basevar)] = 0;
      osi->tos--;
    }
}


/* Initialize data structures for the object size computation.  */

void
init_object_sizes (void)
{
  int object_size_type;

  if (computed[0])
    return;

  for (object_size_type = 0; object_size_type < OST_END; object_size_type++)
    {
      object_sizes_grow (object_size_type);
      computed[object_size_type] = BITMAP_ALLOC (NULL);
    }

  init_offset_limit ();
}


/* Destroy data structures after the object size computation.  */

void
fini_object_sizes (void)
{
  int object_size_type;

  for (object_size_type = 0; object_size_type < OST_END; object_size_type++)
    {
      object_sizes_release (object_size_type);
      BITMAP_FREE (computed[object_size_type]);
    }
}

/* Dummy valueize function.  */

static tree
do_valueize (tree t)
{
  return t;
}

/* Process a __builtin_object_size or __builtin_dynamic_object_size call in
   CALL early for subobjects before any object information is lost due to
   optimization.  Insert a MIN or MAX expression of the result and
   __builtin_object_size at I so that it may be processed in the second pass.
   __builtin_dynamic_object_size is treated like __builtin_object_size here
   since we're only looking for constant bounds.  */

static void
early_object_sizes_execute_one (gimple_stmt_iterator *i, gimple *call)
{
  tree ost = gimple_call_arg (call, 1);
  tree lhs = gimple_call_lhs (call);
  gcc_assert (lhs != NULL_TREE);

  if (!tree_fits_uhwi_p (ost))
    return;

  unsigned HOST_WIDE_INT object_size_type = tree_to_uhwi (ost);
  tree ptr = gimple_call_arg (call, 0);

  if (object_size_type != 1 && object_size_type != 3)
    return;

  if (TREE_CODE (ptr) != ADDR_EXPR && TREE_CODE (ptr) != SSA_NAME)
    return;

  tree type = TREE_TYPE (lhs);
  tree bytes;
  if (!compute_builtin_object_size (ptr, object_size_type, &bytes)
      || !int_fits_type_p (bytes, type))
    return;

  tree tem = make_ssa_name (type);
  gimple_call_set_lhs (call, tem);
  enum tree_code code = object_size_type & OST_MINIMUM ? MAX_EXPR : MIN_EXPR;
  tree cst = fold_convert (type, bytes);
  gimple *g = gimple_build_assign (lhs, code, tem, cst);
  gsi_insert_after (i, g, GSI_NEW_STMT);
  update_stmt (call);
}

/* Attempt to fold one __builtin_dynamic_object_size call in CALL into an
   expression and insert it at I.  Return true if it succeeds.  */

static bool
dynamic_object_sizes_execute_one (gimple_stmt_iterator *i, gimple *call)
{
  gcc_assert (gimple_call_num_args (call) == 2);

  tree args[2];
  args[0] = gimple_call_arg (call, 0);
  args[1] = gimple_call_arg (call, 1);

  location_t loc = EXPR_LOC_OR_LOC (args[0], input_location);
  tree result_type = gimple_call_return_type (as_a <gcall *> (call));
  tree result = fold_builtin_call_array (loc, result_type,
					 gimple_call_fn (call), 2, args);

  if (!result)
    return false;

  /* fold_builtin_call_array may wrap the result inside a
     NOP_EXPR.  */
  STRIP_NOPS (result);
  gimplify_and_update_call_from_tree (i, result);

  if (dump_file && (dump_flags & TDF_DETAILS))
    {
      fprintf (dump_file, "Simplified (dynamic)\n  ");
      print_gimple_stmt (dump_file, call, 0, dump_flags);
      fprintf (dump_file, " to ");
      print_generic_expr (dump_file, result);
      fprintf (dump_file, "\n");
    }
  return true;
}

static unsigned int
object_sizes_execute (function *fun, bool early)
{
  basic_block bb;
  FOR_EACH_BB_FN (bb, fun)
    {
      gimple_stmt_iterator i;
      for (i = gsi_start_bb (bb); !gsi_end_p (i); gsi_next (&i))
	{
	  tree result;
	  bool dynamic = false;

	  gimple *call = gsi_stmt (i);
	  if (gimple_call_builtin_p (call, BUILT_IN_DYNAMIC_OBJECT_SIZE))
	    dynamic = true;
	  else if (!gimple_call_builtin_p (call, BUILT_IN_OBJECT_SIZE))
	    continue;

	  tree lhs = gimple_call_lhs (call);
	  if (!lhs)
	    continue;

	  init_object_sizes ();

	  /* If early, only attempt to fold
	     __builtin_object_size (x, 1) and __builtin_object_size (x, 3),
	     and rather than folding the builtin to the constant if any,
	     create a MIN_EXPR or MAX_EXPR of the __builtin_object_size
	     call result and the computed constant.  Do the same for
	     __builtin_dynamic_object_size too.  */
	  if (early)
	    {
	      early_object_sizes_execute_one (&i, call);
	      continue;
	    }

	  if (dynamic)
	    {
	      if (dynamic_object_sizes_execute_one (&i, call))
		continue;
	      else
		{
		  /* If we could not find a suitable size expression, lower to
		     __builtin_object_size so that we may at least get a
		     constant lower or higher estimate.  */
		  tree bosfn = builtin_decl_implicit (BUILT_IN_OBJECT_SIZE);
		  gimple_call_set_fndecl (call, bosfn);
		  update_stmt (call);

		  if (dump_file && (dump_flags & TDF_DETAILS))
		    {
		      print_generic_expr (dump_file, gimple_call_arg (call, 0),
					  dump_flags);
		      fprintf (dump_file,
			       ": Retrying as __builtin_object_size\n");
		    }
		}
	    }

	  result = gimple_fold_stmt_to_constant (call, do_valueize);
	  if (!result)
	    {
	      tree ost = gimple_call_arg (call, 1);

	      if (tree_fits_uhwi_p (ost))
		{
		  unsigned HOST_WIDE_INT object_size_type = tree_to_uhwi (ost);

		  if (object_size_type & OST_MINIMUM)
		    result = build_zero_cst (size_type_node);
		  else if (object_size_type < OST_END)
		    result = fold_convert (size_type_node,
					   integer_minus_one_node);
		}

	      if (!result)
		continue;
	    }

	  gcc_assert (TREE_CODE (result) == INTEGER_CST);

	  if (dump_file && (dump_flags & TDF_DETAILS))
	    {
	      fprintf (dump_file, "Simplified\n  ");
	      print_gimple_stmt (dump_file, call, 0, dump_flags);
	      fprintf (dump_file, " to ");
	      print_generic_expr (dump_file, result);
	      fprintf (dump_file, "\n");
	    }

	  /* Propagate into all uses and fold those stmts.  */
	  if (!SSA_NAME_OCCURS_IN_ABNORMAL_PHI (lhs))
	    replace_uses_by (lhs, result);
	  else
	    replace_call_with_value (&i, result);
	}
    }

  fini_object_sizes ();
  return 0;
}

/* Simple pass to optimize all __builtin_object_size () builtins.  */

namespace {

const pass_data pass_data_object_sizes =
{
  GIMPLE_PASS, /* type */
  "objsz", /* name */
  OPTGROUP_NONE, /* optinfo_flags */
  TV_NONE, /* tv_id */
  ( PROP_cfg | PROP_ssa ), /* properties_required */
  PROP_objsz, /* properties_provided */
  0, /* properties_destroyed */
  0, /* todo_flags_start */
  0, /* todo_flags_finish */
};

class pass_object_sizes : public gimple_opt_pass
{
public:
  pass_object_sizes (gcc::context *ctxt)
    : gimple_opt_pass (pass_data_object_sizes, ctxt)
  {}

  /* opt_pass methods: */
  opt_pass * clone () { return new pass_object_sizes (m_ctxt); }
  virtual unsigned int execute (function *fun)
  {
    return object_sizes_execute (fun, false);
  }
}; // class pass_object_sizes

} // anon namespace

gimple_opt_pass *
make_pass_object_sizes (gcc::context *ctxt)
{
  return new pass_object_sizes (ctxt);
}

/* Early version of pass to optimize all __builtin_object_size () builtins.  */

namespace {

const pass_data pass_data_early_object_sizes =
{
  GIMPLE_PASS, /* type */
  "early_objsz", /* name */
  OPTGROUP_NONE, /* optinfo_flags */
  TV_NONE, /* tv_id */
  ( PROP_cfg | PROP_ssa ), /* properties_required */
  0, /* properties_provided */
  0, /* properties_destroyed */
  0, /* todo_flags_start */
  0, /* todo_flags_finish */
};

class pass_early_object_sizes : public gimple_opt_pass
{
public:
  pass_early_object_sizes (gcc::context *ctxt)
    : gimple_opt_pass (pass_data_early_object_sizes, ctxt)
  {}

  /* opt_pass methods: */
  virtual unsigned int execute (function *fun)
  {
    return object_sizes_execute (fun, true);
  }
}; // class pass_object_sizes

} // anon namespace

gimple_opt_pass *
make_pass_early_object_sizes (gcc::context *ctxt)
{
  return new pass_early_object_sizes (ctxt);
}
