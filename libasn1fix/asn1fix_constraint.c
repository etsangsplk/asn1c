#include <asn1fix_internal.h>
#include <asn1fix_constraint.h>
#include <asn1fix_crange.h>

static void _remove_exceptions(arg_t *arg, asn1p_constraint_t *ct);
static int _constraint_value_resolve(arg_t *arg, asn1p_module_t *mod, asn1p_value_t **value);

int
asn1constraint_pullup(arg_t *arg) {
	asn1p_expr_t *expr = arg->expr;
	asn1p_constraint_t *ct_parent;
	asn1p_constraint_t *ct_expr;
	int ret;

	if(expr->combined_constraints)
		return 0;	/* Operation already performed earlier */

	switch(expr->meta_type) {
	case AMT_TYPE:
	case AMT_TYPEREF:
		break;
	default:
		return 0;	/* Nothing to do */
	}

	if(expr->expr_type == A1TC_REFERENCE) {
		asn1p_ref_t *ref = expr->reference;
		asn1p_expr_t *parent_expr;

		assert(ref);
		parent_expr = asn1f_lookup_symbol(arg, expr->module, ref);
		if(!parent_expr) {
			if(errno != EEXIST) {
				DEBUG("\tWhile fetching parent constraints: "
					"type \"%s\" not found: %s",
					asn1f_printable_reference(ref),
					strerror(errno));
				return -1;
			} else {
				/*
				 * -fknown-extern-type is given.
				 * Assume there are no constraints there.
				 */
				WARNING("External type \"%s\": "
					"assuming no constraints",
					asn1f_printable_reference(ref));
				ct_parent = 0;
			}
		} else {
			arg->expr = parent_expr;
			ret = asn1constraint_pullup(arg);
			arg->expr = expr;
			if(ret) return ret;

			ct_parent = parent_expr->combined_constraints;
		}
	} else {
		ct_parent = 0;
	}

	ct_expr = expr->constraints;

	if(!ct_parent && !ct_expr)
		return 0;	/* No constraints to consider */

	if(ct_parent) {
		ct_parent = asn1p_constraint_clone(ct_parent);
		assert(ct_parent);
	}

	/*
	 * If the current type does not have constraints, it inherits
	 * the constraints of a parent.
	 */
	if(ct_parent && !ct_expr) {
		expr->combined_constraints = ct_parent;
		return 0;
	}

	ct_expr = asn1p_constraint_clone(ct_expr);
	assert(ct_expr);

	/*
	 * Now we have a set of current expression's constraints,
	 * and an optional set of the parent expression's constraints.
	 */

	if(ct_parent) {
		/*
		 * If we have a parent, remove all the extensions (46.4).
		 */
		_remove_exceptions(arg, ct_parent);

		expr->combined_constraints = ct_parent;
		if(ct_expr->type == ACT_CA_SET) {
			int i;
			for(i = 0; i < ct_expr->el_count; i++) {
				if(asn1p_constraint_insert(
					expr->combined_constraints,
						ct_expr->elements[i])) {
					expr->combined_constraints = 0;
					asn1p_constraint_free(ct_expr);
					asn1p_constraint_free(ct_parent);
					return -1;
				} else {
					ct_expr->elements[i] = 0;
				}
			}
			asn1p_constraint_free(ct_expr);
		} else {
			asn1p_constraint_insert(expr->combined_constraints,
				ct_expr);
		}
	} else {
		expr->combined_constraints = ct_expr;
	}

	return 0;
}

int
asn1constraint_resolve(arg_t *arg, asn1p_module_t *mod, asn1p_constraint_t *ct, asn1p_expr_type_e etype, enum asn1p_constraint_type_e effective_type) {
	int rvalue = 0;
	int ret;
	int el;

	if(!ct) return 0;

	/* Don't touch information object classes */
	switch(ct->type) {
	case ACT_CT_SIZE:
	case ACT_CT_FROM:
		if(effective_type && effective_type != ct->type) {
			FATAL("%s at line %d: "
				"Incompatible nested %s within %s",
				arg->expr->Identifier, ct->_lineno,
				asn1p_constraint_type2str(ct->type),
				asn1p_constraint_type2str(effective_type)
			);
		}
		effective_type = ct->type;
		break;
	case ACT_CT_WCOMP:
	case ACT_CT_WCOMPS:
	case ACT_CA_CRC:
		return 0;
	default:
		break;
	}

	if(etype != A1TC_INVALID) {
		enum asn1p_constraint_type_e check_type;

		check_type = effective_type ? effective_type : ct->type;

		ret = asn1constraint_compatible(etype, check_type);
		switch(ret) {
		case -1:	/* If unknown, assume OK. */
		case  1:
			break;
		case 0:
			if(effective_type == ACT_CT_SIZE
			&& (arg->flags & A1F_EXTENDED_SizeConstraint))
				break;
		default:
			FATAL("%s at line %d: "
				"Constraint type %s is not applicable to %s",
				arg->expr->Identifier, ct->_lineno,
				asn1p_constraint_type2str(check_type),
				ASN_EXPR_TYPE2STR(etype)
			);
			rvalue = -1;
			break;
		}
	} else {
		WARNING("%s at line %d: "
			"Constraints ignored: Unresolved parent type",
			arg->expr->Identifier, arg->expr->_lineno);
	}

	/*
	 * Resolve all possible references, wherever they occur.
	 */
	if(ct->value && ct->value->type == ATV_REFERENCED) {
		ret = _constraint_value_resolve(arg, mod, &ct->value);
		RET2RVAL(ret, rvalue);
	}
	if(ct->range_start && ct->range_start->type == ATV_REFERENCED) {
		ret = _constraint_value_resolve(arg, mod, &ct->range_start);
		RET2RVAL(ret, rvalue);
	}
	if(ct->range_stop && ct->range_stop->type == ATV_REFERENCED) {
		ret = _constraint_value_resolve(arg, mod, &ct->range_stop);
		RET2RVAL(ret, rvalue);
	}

	/*
	 * Proceed recursively.
	 */
	for(el = 0; el < ct->el_count; el++) {
		ret = asn1constraint_resolve(arg, mod, ct->elements[el],
			etype, effective_type);
		RET2RVAL(ret, rvalue);
	}

	return rvalue;
}

static void
_remove_exceptions(arg_t *arg, asn1p_constraint_t *ct) {
	int i;

	for(i = 0; i < ct->el_count; i++) {
		if(ct->elements[i]->type == ACT_EL_EXT)
			break;
		_remove_exceptions(arg, ct->elements[i]);
	}

	/* Remove the elements at and after the extensibility mark */
	for(; i < ct->el_count; ct->el_count--) {
		asn1p_constraint_t *rm;
		rm = ct->elements[ct->el_count-1];
		asn1p_constraint_free(rm);
	}

	if(i < ct->el_size)
		ct->elements[i] = 0;
}


static int
_constraint_value_resolve(arg_t *arg, asn1p_module_t *mod, asn1p_value_t **value) {
	asn1p_expr_t static_expr;
	asn1p_expr_t *tmp_expr;
	arg_t tmp_arg;
	int rvalue = 0;
	int ret;

	tmp_expr = asn1f_lookup_symbol(arg, mod, (*value)->value.reference);
	if(tmp_expr == NULL) {
		FATAL("Cannot find symbol %s "
			"used in %s subtype constraint at line %d",
			asn1f_printable_reference((*value)->value.reference),
			arg->expr->Identifier, arg->expr->_lineno);
		assert((*value)->type == ATV_REFERENCED);
		return -1;
	}

	static_expr = *tmp_expr;
	static_expr.value = *value;
	tmp_arg = *arg;
	tmp_arg.mod = tmp_expr->module;
	tmp_arg.expr = &static_expr;
	ret = asn1f_fix_dereference_values(&tmp_arg);
	RET2RVAL(ret, rvalue);
	assert(static_expr.value);
	*value = static_expr.value;

	return rvalue;
}