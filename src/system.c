#include <assert.h>
#include <string.h>
#include "include/private/reflecs.h"
#include "include/util/time.h"

static
void match_family(
    EcsWorld *world,
    EcsStage *stage,
    EcsEntity system,
    EcsRowSystem *system_data,
    EcsFamily family)
{
    /* Test if the components of the system are equal or a subset of the 
     * components of the family */
    EcsEntity match = ecs_family_contains(
        world, stage, family, system_data->base.and_from_entity, true, false);

    /* If there is a match, add the system to the family-row_system index */
    if (match) {
        EcsMap *index;
        EcsSystemKind kind = system_data->base.kind;

        if (kind == EcsOnAdd) {
            index = world->family_sys_add_index;
        } else if (kind == EcsOnRemove) {
            index = world->family_sys_remove_index;
        } else if (kind == EcsOnSet) {
            index = world->family_sys_set_index;
        }

        EcsArray *systems = ecs_map_get(index, family);
        if (!systems) {
            systems = ecs_array_new(&handle_arr_params, 1);
        }

        EcsEntity *new_elem = ecs_array_add(&systems, &handle_arr_params);
        *new_elem = system;

        /* Always set the system entry, as array may have been realloc'd */
        ecs_map_set(index, family, systems);
    }
}

/* Match system against existing families to build the family-rowsys index */
static
void match_families(
    EcsWorld *world,
    EcsEntity system,
    EcsRowSystem *system_data)
{
    EcsIter it = ecs_map_iter(world->family_index);

    /* Iterating over a map is a bit slow, but this only happens when a new
     * row system is created, which is very infrequent. */
    while (ecs_iter_hasnext(&it)) {
        uint64_t key; /* Only interested in the key, which is the family hash */
        ecs_map_next(&it, &key);

        EcsFamily family = key;
        
        match_family(world, NULL, system, system_data, family);
    }
}

/** Create a new row system. A row system is a system executed on a single row,
 * typically as a result of a ADD, REMOVE or SET trigger.
 */
static
EcsEntity new_row_system(
    EcsWorld *world,
    const char *id,
    EcsSystemKind kind,
    bool needs_tables,
    const char *sig,
    EcsSystemAction action)
{
    uint32_t count = ecs_columns_count(sig);
    if (!count) {
        return 0;
    }

    EcsEntity result = ecs_new_w_family(world, NULL, world->row_system_family);
    EcsId *id_data = ecs_get_ptr(world, result, EcsId_h);
    *id_data = id;

    EcsRowSystem *system_data = ecs_get_ptr(world, result, EcsRowSystem_h);
    memset(system_data, 0, sizeof(EcsRowSystem));
    system_data->base.action = action;
    system_data->base.signature = sig;
    system_data->base.enabled = true;
    system_data->base.kind = kind;
    system_data->components = ecs_array_new(&handle_arr_params, count);

    if (ecs_parse_component_expr(
        world, sig, ecs_parse_component_action, system_data) != EcsOk)
    {
        ecs_abort(ECS_INVALID_COMPONENT_EXPRESSION, sig);
    }

    EcsFamily family_id = 0;
    uint32_t i, column_count = ecs_array_count(system_data->base.columns);
    EcsSystemColumn *buffer = ecs_array_buffer(system_data->base.columns);

    for (i = 0; i < column_count; i ++) {
        EcsEntity *h = ecs_array_add(
            &system_data->components, &handle_arr_params);

        EcsSystemColumn *column = &buffer[i];
        *h = column->is.component;

        if (column->kind != EcsFromId) {
            family_id = ecs_family_add(
                world, NULL, family_id, column->is.component);
        }
    }

    EcsEntity *elem = NULL;

    if (!needs_tables) {
        if (kind == EcsOnFrame) {
            elem = ecs_array_add(&world->tasks, &handle_arr_params);
        } else if (kind == EcsOnRemove) {
            elem = ecs_array_add(&world->fini_tasks, &handle_arr_params);
        }
    } else {
        if (kind == EcsOnAdd) {
            elem = ecs_array_add(&world->add_systems, &handle_arr_params);
        } else if (kind == EcsOnRemove) {
            elem = ecs_array_add(&world->remove_systems, &handle_arr_params);
        } else if (kind == EcsOnSet) {
            elem = ecs_array_add(&world->set_systems, &handle_arr_params);
        }
    }

    if (elem) {
        *elem = result;
    }

    ecs_system_compute_and_families(world, result, &system_data->base);
    match_families(world, result, system_data);

    return result;
}

/* -- Private API -- */

void ecs_system_compute_and_families(
    EcsWorld *world,
    EcsEntity system,
    EcsSystem *system_data)
{
    uint32_t i, column_count = ecs_array_count(system_data->columns);
    EcsSystemColumn *buffer = ecs_array_buffer(system_data->columns);
    EcsColSystem *col_system_data = NULL;

    for (i = 0; i < column_count; i ++) {
        EcsSystemColumn *elem = &buffer[i];
        EcsSystemExprElemKind elem_kind = elem->kind;
        EcsSystemExprOperKind oper_kind = elem->oper_kind;

        if (elem_kind == EcsFromEntity) {
            if (oper_kind == EcsOperAnd) {
                system_data->and_from_entity = ecs_family_add(
                 world, NULL, system_data->and_from_entity, elem->is.component);
            }
        } else if (elem_kind == EcsFromSystem) {
            if (!col_system_data) {
                col_system_data = ecs_get_ptr(world, system, EcsColSystem_h);
                if (!col_system_data) {
                    ecs_abort(ECS_INVALID_COMPONENT_EXPRESSION, NULL);
                }
            }
            if (oper_kind == EcsOperAnd) {
                col_system_data->and_from_system = ecs_family_add(
                 world, NULL, col_system_data->and_from_system, elem->is.component);
            }
        }
    }
}

/** Parse callback that adds component to the components array for a system */
EcsResult ecs_parse_component_action(
    EcsWorld *world,
    EcsSystemExprElemKind elem_kind,
    EcsSystemExprOperKind oper_kind,
    const char *component_id,
    void *data)
{
    EcsSystem *system_data = data;
    EcsSystemColumn *elem;

    /* Lookup component handly by string identifier */
    EcsEntity component = ecs_lookup(world, component_id);
    if (!component) {
        /* "0" is a valid expression used to indicate that a system matches no
         * components */
        if (strcmp(component_id, "0")) {
            ecs_abort(ECS_INVALID_COMPONENT_ID, component_id);
        }
    }

    /* If retrieving a component from a system, only the AND operator is
     * supported. The set of system components is expected to be constant, and
     * thus no conditional operators are needed. */
    if (elem_kind == EcsFromSystem && oper_kind != EcsOperAnd) {
        ecs_abort(ECS_INVALID_COMPONENT_EXPRESSION, 0);
    }

    /* AND (default) and optional columns are stored the same way */
    if (oper_kind == EcsOperAnd || oper_kind == EcsOperOptional) {
        elem = ecs_array_add(&system_data->columns, &column_arr_params);
        elem->kind = elem_kind;
        elem->oper_kind = oper_kind;
        elem->is.component = component;

    /* OR columns store a family id instead of a single component */
    } else if (oper_kind == EcsOperOr) {
        elem = ecs_array_last(system_data->columns, &column_arr_params);
        if (elem->oper_kind == EcsOperAnd) {
            elem->is.family = ecs_family_add(
                world, NULL, 0, elem->is.component);
        } else {
            if (elem->kind != elem_kind) {
                /* Cannot mix FromEntity and FromComponent in OR */
                goto error;
            }
        }

        elem->is.family = ecs_family_add(
            world, NULL, elem->is.family, component);
        elem->kind = elem_kind;
        elem->oper_kind = EcsOperOr;

    /* A system stores two NOT familes; one for entities and one for components.
     * These can be quickly & efficiently used to exclude tables with
     * ecs_family_contains. */
    } else if (oper_kind == EcsOperNot) {
        elem = ecs_array_add(&system_data->columns, &column_arr_params);
        elem->kind = EcsFromId; /* Just pass handle to system */
        elem->oper_kind = EcsOperNot;
        elem->is.component = component;

        if (elem_kind == EcsFromEntity) {
            system_data->not_from_entity =
                ecs_family_add(
                    world, NULL, system_data->not_from_entity, component);
        } else {
            system_data->not_from_component =
              ecs_family_add(
                  world, NULL, system_data->not_from_component, component);
        }
    }

    return EcsOk;
error:
    return EcsError;
}

/** Run system on a single row */
void ecs_row_notify(
    EcsWorld *world,
    EcsEntity system,
    EcsRowSystem *system_data,
    int32_t *columns,
    EcsTableColumn *table_columns,
    uint32_t offset,
    uint32_t limit)
{
    EcsSystemAction action = system_data->base.action;

    EcsRows rows = {
        .world = world,
        .system = system,
        .columns = columns,
        .column_count = ecs_array_count(system_data->components),
        .table_columns = table_columns,
        .components = ecs_array_buffer(system_data->components),
        .index_offset = 0,
        .offset = offset,
        .limit = limit
    };

    action(&rows);
}

/* Notify row system of a new family */
void ecs_row_system_notify_of_family(
    EcsWorld *world,
    EcsStage *stage,
    EcsEntity system,
    EcsFamily family)
{
    EcsRowSystem *system_data = ecs_get_ptr(world, system, EcsRowSystem_h);

    match_family(world, stage, system, system_data, family);
}

/** Run a task. A task is a system that contains no columns that can be matched
 * against a table. Examples of such columns are EcsFromSystem or EcsFromId.
 * Tasks are ran once every frame. */
void ecs_run_task(
    EcsWorld *world,
    EcsEntity system,
    float delta_time)
{
    EcsRowSystem *system_data = ecs_get_ptr(world, system, EcsRowSystem_h);
    assert(system_data != NULL);

    if (!system_data->base.enabled) {
        return;
    }

    bool measure_time = world->measure_system_time;
    struct timespec time_start;
    if (measure_time) {
        ut_time_get(&time_start);
    }

    EcsRows info = {
        .world = world,
        .system = system,
        .delta_time = delta_time,
        .column_count = ecs_array_count(system_data->components),
        .components = ecs_array_buffer(system_data->components),
    };

    system_data->base.action(&info);

    if (measure_time) {
        system_data->base.time_spent += ut_time_measure(&time_start);
    }
}

/* -- Public API -- */

EcsEntity ecs_new_system(
    EcsWorld *world,
    const char *id,
    EcsSystemKind kind,
    const char *sig,
    EcsSystemAction action)
{
    EcsEntity result = ecs_lookup(world, id);
    if (result) {
        return result;
    }

    bool needs_tables = ecs_needs_tables(world, sig);

    if (!needs_tables && (kind == EcsOnAdd ||
        kind == EcsOnSet))
    {
        ecs_abort(ECS_INVALID_PARAMETERS, 0);
    }

    if (needs_tables && (kind == EcsOnFrame || kind == EcsPreFrame ||
                         kind == EcsPostFrame || kind == EcsOnDemand ||
                         kind == EcsOnLoad || kind == EcsOnStore))
    {
        result = ecs_new_col_system(world, id, kind, sig, action);
    } else if (!needs_tables ||
        (kind == EcsOnAdd || kind == EcsOnRemove || kind == EcsOnSet))
    {
        result = new_row_system(world, id, kind, needs_tables, sig, action);
    } else {
        ecs_abort(ECS_INVALID_PARAMETERS, 0);
    }

    return result;
}

void* ecs_set_system_context_ptr(
    EcsWorld *world,
    EcsEntity system,
    EcsEntity component,
    void *value)
{
    EcsSystem *system_data = ecs_get_ptr(world, system, EcsColSystem_h);
    if (!system_data) {
        system_data = ecs_get_ptr(world, system, EcsRowSystem_h);
    }

    system_data->ctx_handle = component;

    ecs_set_ptr(world, system, component, value);
    void *result = ecs_get_ptr(world, system, component);
    assert(result != NULL);

    return result;
}

void* ecs_get_system_context(
    EcsWorld *world,
    EcsEntity system)
{
    EcsSystem *system_data = ecs_get_ptr(world, system, EcsColSystem_h);
    if (!system_data) {
        system_data = ecs_get_ptr(world, system, EcsRowSystem_h);
    }

    void *result = ecs_get_ptr(world, system, system_data->ctx_handle);
    if (!result) {
        ecs_abort(ECS_MISSING_SYSTEM_CONTEXT, 0);
    }

    return result;
}

void ecs_enable(
    EcsWorld *world,
    EcsEntity system,
    bool enabled)
{
    assert(world->magic == ECS_WORLD_MAGIC);
    bool col_system = false;

    /* Try to get either ColSystem or RowSystem data */
    EcsSystem *system_data = ecs_get_ptr(world, system, EcsColSystem_h);
    if (!system_data) {
        system_data = ecs_get_ptr(world, system, EcsRowSystem_h);
    } else {
        col_system = true;
    }

    /* If entity is neither ColSystem nor RowSystem, it should be a family */
    if (!system_data) {
        EcsFamilyComponent *family_data = ecs_get_ptr(
            world, system, EcsFamilyComponent_h);

        assert(family_data != NULL);

        EcsWorld *world_temp = world;
        EcsStage *stage = ecs_get_stage(&world_temp);
        EcsArray *family = ecs_family_get(world, stage, family_data->family);
        EcsEntity *buffer = ecs_array_buffer(family);
        uint32_t i, count = ecs_array_count(family);
        for (i = 0; i < count; i ++) {
            /* Enable/disable all systems in family */
            ecs_enable(world, buffer[i], enabled);
        }
    } else {
        if (col_system) {
            EcsColSystem *col_system = (EcsColSystem*)system_data;
            if (enabled) {
                if (!system_data->enabled) {
                    if (ecs_array_count(col_system->tables)) {
                        ecs_world_activate_system(
                            world, system, col_system->base.kind, true);
                    }
                }
            } else {
                if (system_data->enabled) {
                    if (ecs_array_count(col_system->tables)) {
                        ecs_world_activate_system(
                            world, system, col_system->base.kind, false);
                    }
                }
            }
        }

        system_data->enabled = enabled;
    }
}

bool ecs_is_enabled(
    EcsWorld *world,
    EcsEntity system)
{
    EcsSystem *system_data = ecs_get_ptr(world, system, EcsColSystem_h);
    if (!system_data) {
        system_data = ecs_get_ptr(world, system, EcsRowSystem_h);
    }

    if (system_data) {
        return system_data->enabled;
    } else {
        return true;
    }
}

void ecs_set_period(
    EcsWorld *world,
    EcsEntity system,
    float period)
{
    assert(world->magic == ECS_WORLD_MAGIC);
    EcsColSystem *system_data = ecs_get_ptr(world, system, EcsColSystem_h);
    if (system_data) {
        system_data->period = period;
    }
}

void* ecs_column(
    EcsRows *rows,
    uint32_t index)
{
    if (index > rows->column_count) {
        return NULL;
    }

    uint32_t table_column;

    if (index == 0) {
        table_column = 0;
    } else {
        table_column = rows->columns[index - 1];
    }

    EcsTableColumn *column = &((EcsTableColumn*)rows->table_columns)[table_column];

    return ecs_array_buffer(column->data);
}