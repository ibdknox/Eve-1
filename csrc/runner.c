#include <runtime.h>

#define multibag_foreach(__m, __u, __b)  if(__m) table_foreach(__m, __u, __b)
                         
// debuggin
static estring bagname(evaluation e, uuid u)
{
    
    estring bagname = efalse;
    table_foreach(e->scopes, n, u2) if (u2 ==u) return(n);
    return(intern_cstring("missing bag?")); 
}


static CONTINUATION_1_5(insert_f, evaluation, uuid, value, value, value, multiplicity);
static void insert_f(evaluation s, uuid u, value e, value a, value v, multiplicity m)
{
    bag b;

    s->inserted = true;
    if (!(b = table_find(s->block_solution, u))) {
        table_set(s->block_solution, u, b = create_bag(s->working, u));
    }
    edb_insert(b, e, a, v, m);
}

static CONTINUATION_2_4(shadow, table, listener, value, value, value, multiplicity);
static void shadow(table multibag, listener result, value e, value a, value v, multiplicity m)
{
    boolean s = false;
    if (m > 0) {
        table_foreach(multibag, u, b) 
            if (count_of(b, e, a, v) <0) s = true;
        if (!s) apply(result, e, a, v, m);
    }
}


static void print_multibag(evaluation s, table m)
{ 
    multibag_foreach(m, u, b) {
        prf("%v %d %v %p\n%b\n", bagname(s, u), edb_size(b), u, b, bag_dump(s->h, b));
    }
}
 

static CONTINUATION_1_5(merge_scan, evaluation, int, listener, value, value, value);
static void merge_scan(evaluation ev, int sig, listener result, value e, value a, value v)
{
    listener f_filter = ev->f_solution?cont(ev->working, shadow, ev->f_solution, result):result;
    listener x_filter = ev->t_solution?cont(ev->working, shadow, ev->t_solution, f_filter):f_filter;

    // xxx - currently precluding removes in the event set
    multibag_foreach(ev->ev_solution, u, b) 
        edb_scan(b, sig, result, e, a, v);

    multibag_foreach(ev->persisted, u, b) 
        edb_scan(b, sig, x_filter, e, a, v);

    multibag_foreach(ev->t_solution, u, b) 
        edb_scan(b, sig, f_filter, e, a, v);

    multibag_foreach(ev->f_solution, u, b) 
        edb_scan(b, sig, result, e, a, v);
}

static CONTINUATION_1_0(evaluation_complete, evaluation);
static void evaluation_complete(evaluation s)
{
    if (s->inserted)
        s->pass = true;
    s->non_empty = true;
}


static void merge_multibag_bag(evaluation ev, table *d, uuid u, bag s)
{
    bag bd;
    if (!*d) {
        *d = create_value_table(ev->working);
    }

    if (!(bd = table_find(*d, u))) {
        table_set(*d, u, s); 
    } else {
        bag_foreach(s, e, a, v, c) 
            edb_insert(bd, e, a, v, c);
    }
}

static void merge_bags(evaluation ev, table *d, table s)
{
    if (!s) return;
    if (!*d) {
        *d = s;
        return;
    }
    table_foreach(s, u , b)
        merge_multibag_bag(ev, d, u, b);
}

static void run_block(evaluation ev, heap h, block bk) 
{
    heap bh = allocate_rolling(pages, sstring("block run"));
    bk->ev->block_solution = create_value_table(ev->working);
    bk->ev->non_empty = false;
    bk->ev->inserted = false;
    u64 z = pages->allocated;
    u64 zb = bk->h->allocated;
    ticks start = rdtsc();
    apply(bk->head, h, 0, op_insert, 0);
    apply(bk->head, h, 0, op_flush, 0);
    ev->cycle_time += rdtsc() - start;

    if (bk->ev->non_empty) {
        vector_foreach(bk->finish, i) 
            apply((block_completion)i, true);
        merge_bags(ev, &bk->ev->next_f_solution, bk->ev->block_solution);
    } else {
        vector_foreach(bk->finish, i) 
            apply((block_completion)i, false);
    }
    destroy(bh);
}

static void fixedpoint(evaluation ev)
{
    long iterations = 0;
    boolean t_continue = true;
    vector counts = allocate_vector(ev->working, 10);

    ticks start_time = now();
    ev->t = start_time;
    ev->t_solution =  0;

    // double iteration
    while (t_continue) {
        ev->pass = true;
        t_continue = false;
        ev->next_t_solution =  0;
        ev->f_solution =  0;
        while (ev->pass) {
            ev->pass = false;
            iterations++;
            ev->next_f_solution =  0;
            vector_foreach(ev->blocks, b) run_block(ev, ev->working, b);
            multibag_foreach(ev->next_f_solution, u, b) {
                if (table_find(ev->persisted, u)) {
                    t_continue = true;
                    merge_multibag_bag(ev, &ev->next_t_solution, u, b);
                } else {
                    merge_multibag_bag(ev, &ev->f_solution, u, b);
                }
            }
        }
        merge_bags(ev, &ev->t_solution, ev->next_t_solution);
        vector_insert(counts, box_float((double)iterations));
        iterations = 0;
        ev->t++;
        ev->ev_solution = 0;
    }

    boolean changed_persistent = false;
    // merge but ignore bags not in persisted
    multibag_foreach(ev->t_solution, u, b) {
        bag bd;
        if ((bd = table_find(ev->persisted, u))) {
            bag_foreach((bag)b, e, a, v, c) {
                changed_persistent = true;
                edb_insert(bd, e, a, v, c);
            }
        }
    }

    if (changed_persistent)
         table_foreach(ev->persisted, _, b) 
             table_foreach(((bag)b)->listeners, t, _)
               if (t != ev->run)
                   apply((thunk)t);

    
    // this is a bit strange, we really only care about the
    // non-persisted final state here
    apply(ev->complete, ev->f_solution, ev->counters);

    ticks end_time = now();
    table_set(ev->counters, intern_cstring("time"), (void *)(end_time - start_time));
    table_set(ev->counters, intern_cstring("iterations"), (void *)iterations);

    prf ("fixedpoint in %t seconds, %d blocks, %V iterations, %d input bags, %d output bags\n", 
         end_time-start_time, vector_length(ev->blocks),
         counts, table_elements(ev->scopes),
         ev->t_solution?table_elements(ev->t_solution):0);
    destroy(ev->working);
}

static void clear_evaluation(evaluation ev)
{
    ev->working = allocate_rolling(pages, sstring("event"));
    ev->t++;
    ev->ev_solution = 0;
    ev->t_solution = 0;
    ev->f_solution =  create_value_table(ev->working);
    ev->next_f_solution = create_value_table(ev->working);
    ev->next_t_solution = create_value_table(ev->working);
}

void inject_event(evaluation ev, buffer b, boolean tracing)
{
    buffer desc;
    clear_evaluation(ev);
    vector n = compile_eve(ev->working, b, tracing, &desc);

    // close this block
    vector_foreach(n, i) {
        block b = build(ev, i);
        run_block(ev, ev->working, b);
        apply(b->head, ev->h, 0, op_close, 0);
    }
    ev->ev_solution = ev->next_f_solution;
    fixedpoint(ev);
    table_set(ev->counters, intern_cstring("cycle-time"), (void *)ev->cycle_time);
}

CONTINUATION_1_0(run_solver, evaluation);
void run_solver(evaluation ev)
{
    clear_evaluation(ev);
    fixedpoint(ev);
    table_set(ev->counters, intern_cstring("cycle-time"), (void *)ev->cycle_time);
}

void close_evaluation(evaluation ev) 
{
    table_foreach(ev->persisted, uuid, b) 
        deregister_listener(b, ev->run);

    vector_foreach(ev->blocks, b)
        apply(((block)b)->head, ev->working, 0, op_close, 0);
    
    destroy(ev->h);
}
    
evaluation build_evaluation(table scopes, table persisted, evaluation_result r)
{
    heap h = allocate_rolling(pages, sstring("eval"));
    evaluation ev = allocate(h, sizeof(struct evaluation));
    ev->h = h;
    ev->scopes = scopes;
    // ok, now counts just accrete forever
    ev->counters =  allocate_table(h, key_from_pointer, compare_pointer);
    ev->insert = cont(h, insert_f, ev);
    ev->blocks = allocate_vector(h, 10);
    ev->persisted = persisted;
    ev->cycle_time = 0;
    ev->reader = cont(ev->h, merge_scan, ev);
    ev->complete = r;
    ev->terminal = cont(ev->h, evaluation_complete, ev);

    ev->run = cont(h, run_solver, ev);
    table_foreach(ev->persisted, uuid, b) {
        register_listener(b, ev->run);
        table_foreach(edb_implications(b), n, v){
            vector_insert(ev->blocks, build(ev, n));
        }
    }

    return ev;
}
