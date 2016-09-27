static jl_value_t *intersect_union(jl_value_t *x, jl_uniontype_t *u, jl_stenv_t *e, int8_t R)
{
    jl_unionstate_t *state = R ? &e->Runions : &e->Lunions;
    if (state->depth >= state->stacksize) {
        state->more = 1;
        return 1;
    }
    int ui = statestack_get(state, state->depth);
    state->depth++;
    jl_value_t *choice = ui==0 ? u->a : u->b;
    // try all possible choices in covariant position; union them all together at the top level
    return R ? intersect(x, choice, e, param) : intersect(choice, x, e, param);
}

static jl_value_t *intersect_ufirst(jl_value_t *x, jl_value_t *y, jl_stenv_t *e)
{
    if (jl_is_uniontype(x) && jl_is_typevar(y))
        return intersect_union(y, (jl_uniontype_t*)x, e, 0, 0);
    if (jl_is_typevar(x) && jl_is_uniontype(y))
        return intersect_union(x, (jl_uniontype_t*)y, e, 1, 0));
    return intersect(x, y, e, 0);
}

static jl_value_t *intersect_var(jl_tvar_t *b, jl_value_t *a, jl_stenv_t *e, int param)
{
    jl_varbinding_t *bb = lookup(e, b);
    if (bb == NULL)
        return intersect_ufirst(b->ub, a, e);
    record_var_occurrence(bb, e, param);
    if (!jl_is_type(a) && !jl_is_typevar(a) && bb->lb != jl_bottom_type)
        return jl_bottom_type;
    jl_value_t *ub = intersect_ufirst(bb->ub, a, e);
    bb->ub = ub;
    if (!subtype_ufirst(bb->lb, ub, e))
        return jl_bottom_type;
    return (jl_value_t*)b;
}

static jl_value_t *intersect_unionall(jl_value_t *t, jl_unionall_t *u, jl_stenv_t *e, int8_t R, int param)
{
    jl_varbinding_t *btemp = e->vars;
    // if the var for this unionall (based on identity) already appears somewhere
    // in the environment, rename to get a fresh var.
    // TODO: might need to look inside types in btemp->lb and btemp->ub
    while (btemp != NULL) {
        if (btemp->var == u->var || btemp->lb == (jl_value_t*)u->var ||
            btemp->ub == (jl_value_t*)u->var) {
            u = rename_unionall(u);
            break;
        }
        btemp = btemp->prev;
    }
    jl_varbinding_t vb = { u->var, u->var->lb, u->var->ub, R, NULL, 0, 0, 0, e->invdepth, e->vars };
    JL_GC_PUSH2(&u, &vb.lb);
    e->vars = &vb;
    int ans;
    if (R) {
        e->envidx++;
        ans = subtype(t, u->body, e, param);
        e->envidx--;
        // fill variable values into `envout` up to `envsz`
        if (e->envidx < e->envsz) {
            jl_value_t *val;
            if (vb.lb == vb.ub)
                val = vb.lb;
            else if (vb.lb != jl_bottom_type)
                // TODO: for now return the least solution, which is what
                // method parameters expect.
                val = vb.lb;
            else if (vb.lb == u->var->lb && vb.ub == u->var->ub)
                val = (jl_value_t*)u->var;
            else
                val = (jl_value_t*)jl_new_typevar(u->var->name, vb.lb, vb.ub);
            e->envout[e->envidx] = val;
        }
    }
    else {
        ans = subtype(u->body, t, e, param);
    }

    // handle the "diagonal dispatch" rule, which says that a type var occurring more
    // than once, and only in covariant position, is constrained to concrete types. E.g.
    //  ( Tuple{Int, Int}    <: Tuple{T, T} where T) but
    // !( Tuple{Int, String} <: Tuple{T, T} where T)
    // Then check concreteness by checking that the lower bound is not an abstract type.
    if (ans && (vb.concrete || (!vb.occurs_inv && vb.occurs_cov > 1))) {
        if (jl_is_typevar(vb.lb)) {
            jl_tvar_t *v = (jl_tvar_t*)vb.lb;
            jl_varbinding_t *vlb = lookup(e, v);
            if (vlb)
                vlb->concrete = 1;
            else  // TODO handle multiple variables in vb.concretevar
                ans = (v == vb.concretevar);
        }
        else if (!is_leaf_bound(vb.lb)) {
            ans = 0;
        }
        if (ans) {
            // if we occur as another var's lower bound, record the fact that we
            // were concrete so that subtype can return true for that var.
            btemp = vb.prev;
            while (btemp != NULL) {
                if (btemp->lb == (jl_value_t*)u->var)
                    btemp->concretevar = u->var;
                btemp = btemp->prev;
            }
        }
    }

    e->vars = vb.prev;
    JL_GC_POP();
    return ans;
}

static jl_value_t *intersect_tuple(jl_datatype_t *xd, jl_datatype_t *yd, jl_stenv_t *e)
{
    size_t lx = jl_nparams(xd), ly = jl_nparams(yd);
    if (lx == 0 && ly == 0)
        return (jl_value_t*)yd;
    jl_svec_t *params = jl_alloc_svec(lx > ly ? lx : ly);
    JL_GC_PUSH1(&params);
    size_t i=0, j=0;
    int vx=0, vy=0;
    while (1) {
        jl_value_t *xi = i < lx ? jl_tparam(xd, i) : NULL;
        jl_value_t *yi = j < ly ? jl_tparam(yd, j) : NULL;
        if (xi == NULL && yi == NULL) {
            jl_value_t *res = jl_apply_tuple_type(params);
            JL_GC_POP();
            return res;
        }
        if (xi && jl_is_vararg_type(xi)) vx = 1;
        if (yi && jl_is_vararg_type(yi)) vy = 1;
        if (vx && !vy)
            return 0;
        if (!vx && vy) {
            if (!subtype(xi, jl_unwrap_vararg(yi), e, 1))
                return 0;
        }
        else {
            if (!subtype(xi, yi, e, 1))
                return 0;
        }
        i++;
        if (j < ly-1 || !vy)
            j++;
    }
    // TODO: handle Vararg with explicit integer length parameter
    vy = vy || (j < ly && jl_is_vararg_type(jl_tparam(yd,j)));
    if (vy && !vx && lx+1 >= ly) {
        jl_tvar_t *va_p1=NULL, *va_p2=NULL;
        jl_value_t *tail = unwrap_2_unionall(jl_tparam(yd,ly-1), &va_p1, &va_p2);
        assert(jl_is_datatype(tail));
        // in Tuple{...,tn} <: Tuple{...,Vararg{T,N}}, check (lx+1-ly) <: N
        jl_value_t *N = jl_tparam1(tail);
        // only do the check if N is free in the tuple type's last parameter
        if (N != (jl_value_t*)va_p1 && N != (jl_value_t*)va_p2) {
            if (!subtype(jl_box_long(lx+1-ly), N, e, 1))
                return 0;
        }
    }
    /*
      Tuple{A^n...,Vararg{T,N}} ∩ Tuple{Vararg{S,M}} =
        Tuple{(A∩S)^n...,Vararg{T∩S,N}} plus N = M-n
    */
    return (lx==ly && vx==vy) || (vy && (lx >= (vx ? ly : (ly-1))));
}

// `param` means we are currently looking at a parameter of a type constructor
// (as opposed to being outside any type constructor, or comparing variable bounds).
// this is used to record the positions where type variables occur for the
// diagonal rule (record_var_occurrence).
static int subtype(jl_value_t *x, jl_value_t *y, jl_stenv_t *e, int param)
{
    if (x == jl_ANY_flag) x = (jl_value_t*)jl_any_type;
    if (y == jl_ANY_flag) y = (jl_value_t*)jl_any_type;
    if (jl_is_typevar(x)) {
        if (jl_is_typevar(y)) {
            if (x == y) return 1;
            jl_varbinding_t *xx = lookup(e, (jl_tvar_t*)x);
            jl_varbinding_t *yy = lookup(e, (jl_tvar_t*)y);
            int xr = xx && xx->right;  // treat free variables as "forall" (left)
            int yr = yy && yy->right;
            if (xr) {
                if (yy) record_var_occurrence(yy, e, param);
                if (yr) {
                    // this is a bit odd, but seems necessary to make this case work:
                    // (UnionAll x<:T<:x Ref{Ref{T}}) == Ref{UnionAll x<:T<:x Ref{T}}
                    return subtype(yy->ub, yy->lb, e, 0);
                }
                return var_lt((jl_tvar_t*)x, y, e, param);
            }
            else if (yr) {
                if (xx) record_var_occurrence(xx, e, param);
                return var_gt((jl_tvar_t*)y, x, e, param);
            }
            jl_value_t *xub = xx ? xx->ub : ((jl_tvar_t*)x)->ub;
            jl_value_t *ylb = yy ? yy->lb : ((jl_tvar_t*)y)->lb;
            // check ∀x,y . x<:y
            // the bounds of left-side variables never change, and can only lead
            // to other left-side variables, so using || here is safe.
            return subtype(xub, y, e, param) || subtype(x, ylb, e, param);
        }
        return var_lt((jl_tvar_t*)x, y, e, param);
    }
    if (jl_is_typevar(y))
        return var_gt((jl_tvar_t*)y, x, e, param);
    if (jl_is_uniontype(y)) {
        if (x == y || x == ((jl_uniontype_t*)y)->a || x == ((jl_uniontype_t*)y)->b)
            return 1;
        if (jl_is_unionall(x))
            return subtype_unionall(y, (jl_unionall_t*)x, e, 0, param);
        return subtype_union(x, y, e, 1, &e->Runions, param);
    }
    if (jl_is_uniontype(x)) {
        if (jl_is_unionall(y))
            return subtype_unionall(x, (jl_unionall_t*)y, e, 1, param);
        return subtype_union(y, x, e, 0, &e->Lunions, param);
    }
    if (jl_is_unionall(y)) {
        if (x == y && !(e->envidx < e->envsz))
            return 1;
        return subtype_unionall(x, (jl_unionall_t*)y, e, 1, param);
    }
    if (jl_is_unionall(x))
        return subtype_unionall(y, (jl_unionall_t*)x, e, 0, param);
    if (jl_is_datatype(x) && jl_is_datatype(y)) {
        if (x == y) return 1;
        if (y == (jl_value_t*)jl_any_type) return 1;
        jl_datatype_t *xd = (jl_datatype_t*)x, *yd = (jl_datatype_t*)y;
        if (jl_is_type_type(xd) && !jl_is_typevar(jl_tparam0(xd)) && jl_typeof(jl_tparam0(xd)) == yd)
            // TODO this is not strictly correct, but we don't yet have any other way for
            // e.g. the argument `Int` to match a `::DataType` slot. Most correct would be:
            // Int isa DataType, Int isa Type{Int}, Type{Int} more specific than DataType,
            // !(Type{Int} <: DataType), !isleaftype(Type{Int}), because non-DataTypes can
            // be type-equal to `Int`.
            return 1;
        while (xd != jl_any_type && xd->name != yd->name)
            xd = xd->super;
        if (xd == (jl_value_t*)jl_any_type) return 0;
        if (jl_is_tuple_type(xd))
            return subtype_tuple(xd, yd, e);
        if (jl_is_vararg_type(xd)) {
            // Vararg: covariant in first parameter, invariant in second
            jl_value_t *xp1=jl_tparam0(xd), *xp2=jl_tparam1(xd), *yp1=jl_tparam0(yd), *yp2=jl_tparam1(yd);
            // in Vararg{T1} <: Vararg{T2}, need to check subtype twice to
            // simulate the possibility of multiple arguments, which is needed
            // to implement the diagonal rule correctly.
            if (!subtype(xp1, yp1, e, 1)) return 0;
            if (!subtype(xp1, yp1, e, 1)) return 0;
            // Vararg{T,N} <: Vararg{T2,N2}; equate N and N2
            e->invdepth++;
            int ans = subtype(xp2, yp2, e, 1) && subtype(yp2, xp2, e, 0);
            e->invdepth--;
            return ans;
        }
        size_t i, np = jl_nparams(xd);
        int ans = 1;
        e->invdepth++;
        jl_value_t *ii = y;
        for (i=0; i < np; i++) {
            // just check that the parameters are equal
            jl_value_t *xi = jl_tparam(xd, i), *yi = jl_tparam(yd, i);
            if (!(subtype(xi, yi, e, 0) && subtype(yi, xi, e, 0))) {
                ii = jl_bottom_type;
                break;
            }
        }
        e->invdepth--;
        return ii;
    }
    if (jl_is_type(y))
        return x == jl_bottom_type;
    return x == y || jl_egal(x, y);
}
