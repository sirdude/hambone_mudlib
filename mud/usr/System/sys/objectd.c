/* Released into the public domain Jan 2002 by Noah Gibbs.
   Everything in this file is in the public domain, and is
   released without warranty, guarantee, or even the implication
   of marketability or fitness for a particular purpose.
   If you start a war in Asia with this, don't come crying to
   me. */

/* Thanks go to Geir Harald Hansen, who released an objectd well
   before I knew anything about DGD.  Studying his work has been
   valuable, as has correspondence (his and others) on the DGD Mailing
   List.  This objectd has somewhat different goals than his, and
   operates in some very different (and some very similar) ways.  See
   also the doc/design/OBJECTD document for details. */

#include <kernel/kernel.h>
#include <kernel/objreg.h>
#include <kernel/rsrc.h>
#include <kernel/user.h>

#include <status.h>
#include <type.h>
#include <trace.h>

/** @todo  (features)
   - Allow recompiles that destruct and rebuild all necessary stuff
     - By specifying a list of objects to (re)compile
     - By specifying a list of files that changed
       - Allow to add manually to dependency list?
       - Compile list automatically by time, or just specify time?
*/

inherit objreg API_OBJREG;
inherit rsrc API_RSRC;

/* With aggro_recompile when a parent is found that we don't have an
   issue for, we'll recompile it.  That'll keep all the issue IDs the
   same and stuff, but means we'll have an issue for it.
*/
private int     aggro_recompile;

/* Keep track of recomp_paths and all_libs to see what to recompile
   during init's aggressive recompile.  Untracked_libs is the set of
   libraries that existed during ObjectD's init, so there's one
   untracked issue of each to be removed. */
private mapping recomp_paths, all_libs, untracked_libs;

/* For compiling(), include(), compiled_failed(), etc we need to
   track files.  This mechanism attempts to track what's being
   compiled. */
private string last_file;
private mixed* comp_dep;

/* Used when initializing objectd */
private int setup_done;

/* Should be private so other objects can't get objects by issue --
   that'll be important for security reasons.  These heavy arrays should
   have absolutely no exported references anywhere outside this object,
   ever. */
private object obj_issues;

/* This mapping contains objects whose latest issue is destroyed.  It
   is indexed by object name. */
private mapping dest_issues;

/* Array of objects to upgrade -- normally there should be no more than
   one object here at a time unless a previous call_out has failed... */
private object* upgrade_clonables;
private int     upgrade_callout;

/* This object has a path_special() method.  ObjectD defers to it, if
   it has been set. */
private object path_special_object;

private void   unregister_inherit_data(object issue);
private void   register_inherit_data(object issue);
private void   call_upgraded(object obj);
private object add_clonable(string owner, object obj, string* inherited);
private object add_lib(string owner, string path, string* inherited);
string destroyed_obj_list(void);
private void   suspend_system(void);
private void   release_system(void);


/* These objects are used for tracking issues of objects */


static void create(varargs int clone)
{
    object test;

    objreg::create();
    rsrc::create();

    if(clone) {
	error("Attempting to clone objectd!");
    }
    /* Make a new heavy array to hold the object issues */
    if(!find_object(HEAVY_ARRAY))
	compile_object(HEAVY_ARRAY);
    obj_issues = clone_object(HEAVY_ARRAY);
    dest_issues = ([ ]);
    setup_done = 0;

    aggro_recompile = 0;
    recomp_paths = ([ ]);
    all_libs = ([ ]);
    comp_dep = ({ });
    upgrade_clonables = ({ });
    LOGD->log("before main finds", "objectd");
    if(!find_object(ISSUE_LWO))
	compile_object(ISSUE_LWO);
    LOGD->log("after issue_lwo compile", "objectd");
    if(!find_object(CLONABLE_LWO))
	compile_object(CLONABLE_LWO);
    LOGD->log("after issue_clonable_lwo compile", "objectd");
    if(!find_object(LIB_LWO))
	compile_object(LIB_LWO);

    find_object(DRIVER)->set_object_manager(this_object());        /* become object manager */
}

void destructed(int clone) {
    if(!SYSTEM())
	return;

    destruct_object(obj_issues);
}


/** Private funcs */

/* Used during init.  Recompile all libs we know only by path (not issue
   struct) to get an issue for them. */
private void recompile_to_track_libs(mapping fixups) {
    while(map_sizeof(recomp_paths) > 0) {
	int    ctr;
	mixed* keys;

	keys = map_indices(recomp_paths);
	for(ctr = 0; ctr < sizeof(keys); ctr++) {
	    /* Make sure this gets an entry in the fixups array so we
	       know to fix up the reference to the string afterward. */
	    if(!fixups[keys[ctr]]) fixups[keys[ctr]] = ({ });
	    fixups[keys[ctr]] += recomp_paths[keys[ctr]];
	    recomp_paths[keys[ctr]] = nil;

	    /* Destroy & Recompile lib */
	    destruct_object(keys[ctr]);
	    compile_object(keys[ctr]);
	}
    }
}

/* Used during init.  We've recompiled all the libs, now we need to
   make sure that stuff compiled before had issue objects for the libs
   have the right things in their parent arrays. */
/** @todo add fixup to child arrays?  Or make obsolete? */
private void fix_parent_arrays(mapping fixups) {
    mixed* keys;
    int    ctr;

    keys = map_indices(fixups);
    for(ctr = 0; ctr < sizeof(keys); ctr++) {
	int    ctr2;
	mixed* paths;
	int    lib_index;

	/* For each library to fix up... */
	lib_index = status(keys[ctr])[O_INDEX];

	paths = fixups[keys[ctr]];
	for(ctr2 = 0; ctr2 < sizeof(paths); ctr2++) {
	    string path;
	    object obj, issue;
	    int    ctr3;
	    mixed* inh;

	    /* For each file to fix the lib for... */
	    path = paths[ctr2];
	    obj = find_object(path);
	    if(obj) {
		issue = obj_issues->index(status(obj)[O_INDEX]);
	    } else {
		issue = obj_issues->index(status(path)[O_INDEX]);
	    }

	    /* Replace the string with the issue in the parent array */
	    if(!issue) {
		LOGD->log("Can't get parent issue for " + path, "objectd");
		continue;
	    }
	    inh = issue->get_parents();
	    for(ctr3 = 0; ctr3 < sizeof(inh); ctr3++) {
		if(inh[ctr3] == keys[ctr]) {
		    inh[ctr3] = lib_index;
		}
	    }
	}
    }
}

/* Called during init to fix up child arrays and clear old issues out
   of prev pointers. */
private void fix_child_arrays(string* owners) {
    mixed* keys;
    int    ctr, status_idx;
    object issue, index, first;
    string owner;

    keys = map_indices(all_libs);
    for(ctr = 0; ctr < sizeof(keys); ctr++) {
	issue = obj_issues->index(all_libs[keys[ctr]]);
	if(!issue) {
	    LOGD->log("Can't find issue for index " + all_libs[keys[ctr]],
	      "objectd");
	    error("Can't find issue for index " + all_libs[keys[ctr]]);
	}
	issue->clear_prev();
	issue->clear_children();
    }
    for(ctr = 0; ctr < sizeof(keys); ctr++) {
	issue = obj_issues->index(all_libs[keys[ctr]]);
	if(!issue) {
	    LOGD->log("Can't find issue for index " + all_libs[keys[ctr]],
	      "objectd");
	    error("Can't find issue for index " + all_libs[keys[ctr]]);
	}
	register_inherit_data(issue);
    }

    /* There are two objects in the Kernel Library - TELNET_CONN and
       BINARY_CONN - that ObjRegD does not list. */
    status_idx = status(find_object(TELNET_CONN))[O_INDEX];
    issue = obj_issues->index(status_idx);
    register_inherit_data(issue);

    status_idx = status(find_object(BINARY_CONN))[O_INDEX];
    issue = obj_issues->index(status_idx);
    register_inherit_data(issue);

    /* Iterate through all owners to make sure prev fields are clear and
       to register their inherit data. */
    for(ctr = 0; ctr < sizeof(owners); ctr++) {
	owner = owners[ctr];

	first = objreg::first_link(owner);
	if(!first) continue;  /* No objects for this owner... */

	index = first;
	do {
	    status_idx = status(index)[O_INDEX];
	    issue = obj_issues->index(status_idx);
	    if(!issue || issue->get_prev() != -1) {
		LOGD->log("Can't verify issue in get_child_arrays!",
		  "objectd");
		error("Internal error!");
	    }
	    register_inherit_data(issue);

	    index = objreg::next_link(index);
	} while(index != first);
    }
}

/* Used during init.  Recompile every clonable object */
private void recompile_every_clonable(string* owners) {
    string  owner;
    int     ctr, ctr2;
    object  index, first;
    object* obj_arr;

    /* These are special objects that ObjRegD doesn't contain. */
    compile_object(TELNET_CONN);
    compile_object(BINARY_CONN);

    /* First, for all owners recompile all owned (non-lib) objects. */
    for(ctr = 0; ctr < sizeof(owners); ctr++) {
	owner = owners[ctr];

	first = objreg::first_link(owner);
	if(!first) continue;  /* No objects for this owner... */

	/* Build simple linear object array from circular list in
	   objregd */
	obj_arr = ({ first });
	index = objreg::next_link(first);
	while(index != first) {
	    obj_arr += ({ index });
	    index = objreg::next_link(index);
	}

	/* For this owner, recompile all his objects so we can get their
	   dependencies */
	for(ctr2 = 0; ctr2 < sizeof(obj_arr); ctr2++) {
	    string name, path;

	    name = object_name(obj_arr[ctr2]);

	    /* Skip all clones -- we're just getting dependencies */
	    if(sscanf(name, "%*s#%*d"))
		continue;

	    compile_object(name);
	}
    }
}

/* Used during init.  Recompile every clonable so we know the issue
   info for each one.  While we're doing it, collect inheritance info
   so we know what they inherit from.  Then, recompile all libraries
   to track them properly. */
private void recompile_every_object(string* owners) {
    mapping fixups;
    string *lib_names;
    int     ctr;

    /* set aggressive recompiles -- need to find all libraries */
    aggro_recompile = 1;

    /* Recompile all the clonables so we can get a first set of libs */
    recompile_every_clonable(owners);

    /* Now recompile all the libs we found while compiling but haven't started
       tracking yet */
    fixups = ([ ]);
    recompile_to_track_libs(fixups);

    fix_parent_arrays(fixups);
    /* Now all_libs exists */

    /* Set up untracked_libs */
    lib_names = map_indices(all_libs);
    untracked_libs = ([ ]);
    for(ctr = 0; ctr < sizeof(lib_names); ctr++) {
	LOGD->log("Adding " + lib_names[ctr] + " to untracked_libs",
	  "objectd");
	untracked_libs[lib_names[ctr]] = 1;
    }

    fix_child_arrays(owners);

    /* Add faked AUTO object */
    add_lib("System", AUTO, ({ }));

    /* Set aggro_recompile mode to strong checking -- anything that
       would initiate an aggressive recompile before should be outlawed
       now since we should already know everything we need to know about
       all objects.  This also turns off recomp_paths registration, so
       we can get rid of it. */
    aggro_recompile = 2;
    recomp_paths = nil;
}

/* Used during init.  Objregd keeps track of all clones, so we can count
   them pretty easily. */
private void count_clones(string* owners) {
    string  owner;
    int     ctr, ctr2, ctr3;
    object  index, first;
    object* obj_arr;
    mapping lib_issues;

    lib_issues = ([ ]);

    /* Now, for all owners go through all objects */
    for(ctr = 0; ctr < sizeof(owners); ctr++) {
	owner = owners[ctr];

	first = objreg::first_link(owner);
	if(!first) continue;  /* No objects for this owner... */

	/* Build simple linear object array from circular list in
	   objregd */
	obj_arr = ({ first });
	index = objreg::next_link(first);
	while(index != first) {
	    obj_arr += ({ index });
	    index = objreg::next_link(index);
	}

	/* For this owner, enumerate all objects */
	for(ctr2 = 0; ctr2 < sizeof(obj_arr); ctr2++) {
	    string name, path;
	    int    index;
	    object issue;
	    mixed* inh;

	    name = object_name(obj_arr[ctr2]);
	    if(sscanf(name, "%s#%*d", path)) {

		/* For clones, add to clonables */
		index = status(find_object(path))[O_INDEX];
		issue = obj_issues->index(index);
		issue->add_clone(obj_arr[ctr2]);
	    }
	}
    }
}

/* This removes this issue from the child lists of everybody it
   used to inherit from */
private void unregister_inherit_data(object issue) {
    mixed* parents;
    int    ctr, index;
    object inh_issue;

    index = issue->get_index();
    parents = issue->get_parents();
    for(ctr = 0; ctr < sizeof(parents); ctr++) {
	if(typeof(parents[ctr]) == T_STRING) {
	    if(aggro_recompile > 1) {
		LOGD->log("Uncorrected parent string '" + parents[ctr]
		  + "'!","objectd");
	    }
	} else {
	    inh_issue = obj_issues->index(parents[ctr]);
	    if(!inh_issue) {
		LOGD->log("Internal error (" + issue->get_name()
		  + "), Parent: " + parents[ctr]
		  + ", Child: " + index, "objectd");
		error("Internal error!");
	    }

	    inh_issue->remove_child(index);
	}
    }
}

/* This removes this issue from the parent lists of everybody that
   used to inherit from it.  Currently only used in remove_program. */
private void clear_child_data(object issue) {
    mixed* children;
    int    ctr, index;
    object inh_issue;

    index = issue->get_index();
    children = issue->get_children();
    if(!children) {
	LOGD->log("Should children be NULL for issue #" +
	  index + " in clear_child_data?",
	  "objectd");
	return;
    }
    for(ctr = 0; ctr < sizeof(children); ctr++) {
	inh_issue = obj_issues->index(children[ctr]);
	if(!inh_issue) {
	    LOGD->log("Internal error, Child: " + children[ctr]
	      + ", Parent: " + index, "objectd");
	    error("Internal error!");
	}

	inh_issue->remove_parent(index);
    }
}

/* This adds the issue to the child lists of everybody it now
   inherits from. */
private void register_inherit_data(object issue) {
    mixed* parents;
    int    ctr, index;
    object tmp;

    index = issue->get_index();
    parents = issue->get_parents();
    for(ctr = 0; ctr < sizeof(parents); ctr++) {
	if(typeof(parents[ctr]) == T_STRING) {
	    if(aggro_recompile > 1) {
		LOGD->log("Uncorrected parent string! [2]", "objectd");
	    }
	} else if(typeof(parents[ctr]) == T_NIL) {
	    LOGD->log("Parents[" + ctr + "] is nil for issue #" + index
	      + "!", "objectd");
	} else {
	    tmp = obj_issues->index(parents[ctr]);
	    if(!tmp) {
		LOGD->log("Can't get parent issue from index!", "objectd");
	    } else {
		tmp->add_child(index);
	    }
	}
    }
}

/* Transfer clones from an old object issue to a new one */
private void transfer_clones(object old_issue, object new_issue) {
    if(old_issue->get_num_clones() > 0) {
	new_issue->clones_from(old_issue);
    }

    if(old_issue->get_prev() != -1) {
	transfer_clones(obj_issues->index(old_issue->get_prev()), new_issue);
    }
}

private void convert_inherited_str_to_mixed(string *inherited, mixed* inh_obj, string path) {
    int    tmp_idx, ctr;
    object tmp_issue;

    /* Convert inherited (an array of strings) to inh_obj (an array of
       objects) */
    for(ctr = 0; ctr < sizeof(inherited); ctr++) {
	/* Get latest issue of obj inherited[ctr] */
	tmp_idx = status(inherited[ctr])[O_INDEX];
	tmp_issue = obj_issues->index(tmp_idx);

	if(tmp_issue) {
	    inh_obj[ctr] = tmp_idx;

	    /* During init or later */
	    if(aggro_recompile == 1) {
		all_libs[inherited[ctr]] = tmp_idx;
	    }
	} else {
	    if(aggro_recompile > 1) {
		LOGD->log("Unrecognized lib " + inherited[ctr]
		  + " with strong recompile check on!",
		  "objectd");
	    }
	    if(aggro_recompile == 1) {
		if(!recomp_paths[inherited[ctr]]) recomp_paths[inherited[ctr]] = ({ });
		recomp_paths[inherited[ctr]] += ({ path });
	    }
	    inh_obj[ctr] = inherited[ctr];
	}
    }
}

private object add_clonable(string owner, object obj, string* inherited) {
    object new_issue, old_version, tmp;
    int    idx, ctr, old_index;
    mixed* inh_obj;

    if(!obj) {
	LOGD->log("Nil object passed to add_clonable!", "objectd");
	return nil;
    }

    if(sscanf(object_name(obj), "%*s#%*d")) {
	LOGD->wlog("Clone passed to add_clonable!", "objectd");
	return nil;
    }

    inh_obj = allocate(sizeof(inherited));
    convert_inherited_str_to_mixed(inherited, inh_obj, object_name(obj));

    idx = status(obj)[O_INDEX];

    old_index = -1;
    old_version = obj_issues->index(idx);
    if(old_version) {
	old_index = idx;
    } else if(dest_issues[object_name(obj)]) {
	old_index = dest_issues[object_name(obj)];
	old_version = obj_issues->index(old_index);
	if(!old_version) {
	    LOGD->log("Can't get issue# for destroyed version!", "objectd");
	}
	if(old_version && !old_version->destroyed()) {
	    LOGD->log("Old issue is in dest_issues but not destroyed!",
	      "objectd");
	}
    }

    if(old_version && !old_version->destroyed()) {
	/* Recompile - owner, obj, idx, and old_version stay the
	   same, but inheritance may have changed. */

	unregister_inherit_data(old_version);

	old_version->set_parents(inh_obj);
	register_inherit_data(old_version);

	/*LOGD->log("Upgrading object on recompile", "objectd");*/

	call_upgraded(find_object(object_name(obj)));
    } else {
	/* new object or old one was destructed */
	new_issue = new_object(CLONABLE_LWO);

	/* The clones will all be updated at the end of this thread, so
	   we don't actually need a previous version around. */
	new_issue->set_vals(owner, obj, idx, inh_obj, comp_dep);
	comp_dep = nil;

	obj_issues->set_index(idx, new_issue);
	register_inherit_data(new_issue);

	/* When we recompile this, it'll upgrade the old clones to the new
	   version -- so the destroyed one should go away. */
	if(old_version) {
	    /* Transfer clones over */
	    transfer_clones(old_version, new_issue);

	    /* Remove from dest array */
	    dest_issues[object_name(obj)] = nil;
	    dest_issues[old_index] = nil;
	}

    }

    /* If we're making a new object, that means that this issue# shouldn't
       be in the destructed issues. */
    if(dest_issues[idx]) {
	LOGD->log("Clearing destructed issue# which should be clean!",
	  "objectd");
  
    dest_issues[idx] = nil;
    }
    return new_issue;
}

private object add_lib(string owner, string path, string* inherited) {
    object new_issue, tmp_obj, old_version;
    int    idx, ctr, tmp_idx, old_index;
    mixed* inh_obj;

    inh_obj = allocate(sizeof(inherited));
    convert_inherited_str_to_mixed(inherited, inh_obj, path);

    idx = status(path)[O_INDEX];
    all_libs[path] = idx;
    old_index = -1;
    old_version = obj_issues->index(idx);
    if(old_version) {
	old_index = idx;
    } else if(dest_issues[path]) {
	old_index = dest_issues[path];
	old_version = obj_issues->index(old_index);
	if(!old_version) {
	    LOGD->log("Can't get issue for old_version adding lib!",
	      "objectd");
	}
    }

    if(old_version && !old_version->destroyed()) {
	/* Recompile - owner, path, idx, old_version and mod_count stay the
	   same, but inheritance may have changed. */

	unregister_inherit_data(old_version);

	old_version->set_parents(inh_obj);
	register_inherit_data(old_version);
    } else {
	/* Brand new, or old issue was destructed */
	new_issue = new_object(LIB_LWO);
	/* If old one was destructed then give it as the prev version and
	   set the mod count one higher than the old one */
	new_issue->set_vals(owner, path, idx, inh_obj,
	  comp_dep, old_index);
	comp_dep = nil;

	obj_issues->set_index(idx, new_issue);
	register_inherit_data(new_issue);

	/* If we have a prev version then it's kept track of by the new
	   issue so we can remove it from dest_issues. */
	if(old_version) {
	    dest_issues[path] = nil;
	    dest_issues[old_index] = nil;
	    dest_issues[idx] = nil;
	}
    }

    return new_issue;
}

/** @todo support this functionality in lib */
private void call_upgraded(object obj) {

    if(!obj) {
	LOGD->log("Calling call_upgraded on nil!", "objectd");
	return;
    }

    /* Originally we checked to see if the object had an "upgraded"
       function and if so, we scheduled a callout.  That doesn't work
       because the object hasn't been recompiled yet so it may have just
       gotten or lost an "upgraded" function.  So we just schedule the
       callout and *then* see if it has that function. */
    upgrade_clonables += ({ obj });

    if(!upgrade_callout) {
	suspend_system();
	upgrade_callout = call_out("do_upgrade", 0.0, obj);
	if(upgrade_callout <= 0) {
	    release_system();
	    LOGD->log("Error scheduling upgrade call_out for "
	      + object_name(obj) + "!", "objectd");
	}
    }
}

static void do_upgrade(object obj) {
    int ctr;

    upgrade_callout = 0;

    /* Give objects more time to call upgraded() if they need it since
       they may be rereading large files all at once.  Currently this
       doesn't vary per user. */
    rsrc::set_rsrc("upgrade ticks",
      rsrc::query_rsrc("ticks")[RSRC_MAX] * 4,
      0, 0);

    rlimits(status()[ST_STACKDEPTH]; -1) {
	for(ctr = 0; ctr < sizeof(upgrade_clonables); ctr++) {
	    if(function_object("upgraded", upgrade_clonables[ctr])) {
		catch {
		    rlimits(status()[ST_STACKDEPTH];
		      rsrc::query_rsrc("upgrade ticks")[RSRC_MAX]) {
			call_other(upgrade_clonables[ctr], "upgraded");
		    }
		} : {
		    LOGD->log("Error in " + object_name(upgrade_clonables[ctr])
		      + "->upgraded()! (error text already in log)",
		      "objectd");
		}

	    }
	}
    }

    release_system();
    upgrade_clonables = ({ });
}


/** Hook funcs called after this object is set_object_manager'd */

/* Non-lib object has just been compiled - called just before create() */
void compile(string owner, object obj, string *source, string inherited...)
{
    if(previous_program() == DRIVER) {
	/*LOGD->log("compile: " + object_name(obj)
	  + ", issue #" + status(obj)[O_INDEX], "objectd");*/

	add_clonable(owner, obj, inherited);
    }
}

/* Inheritable object has just been compiled */
void compile_lib(string owner, string path, string *source,
string inherited...)
{
    if(previous_program() == DRIVER) {
	/*LOGD->log("compile_lib: " + path + " ("
	  + status(path)[O_INDEX] + ")", "objectd");*/

	add_lib(owner, path, inherited);
    }
}


/* Object has just been cloned - called just before create(1) */
void clone(string owner, object obj)
{
    if(previous_program() == DRIVER) {
	int index;
	string caught;

	object issue;
	if(!sscanf(object_name(obj), "/kernel/%s", caught)){
	    /*LOGD->log("clone: " + object_name(obj), "objectd");*/

	    index = status(obj)[O_INDEX];
	    /*LOGD->log("index = "+index + " size = "+obj_issues->query_size(), "objectd");*/
	    issue = obj_issues->index(index);
	    /*LOGD->log("function_object('index', obj_issues) = "+function_object("index", obj_issues), "objectd");*/
	    caught = catch(issue->add_clone(obj));
	    if(caught)LOGD->log("issue->add_clone(obj) failed, obj="+object_name(obj)+" Caught: "+caught, "objectd");
	    /* butched in catch to let it run */
	    if(issue->destroyed() && aggro_recompile > 1)
		LOGD->log("Clone of destroyed object...  Odd!", "objectd");
	}
    }
}

/* Object (non-lib) is about to be destructed */
void destruct(string owner, object obj)
{
    if(previous_program() == DRIVER) { 
	int    index;
	object issue;
	string objname;

	/*LOGD->log("destruct: " + object_name(obj)
	  + ", issue #" + status(obj)[O_INDEX], "objectd");*/

	obj->destruct();/* get rid of it from world */		       
	index = status(obj)[O_INDEX];
	issue = obj_issues->index(index);

	if(sscanf(object_name(obj), "%s#%*d", objname)) {
	    if(issue)
		issue->destroy_clone(obj);

	    if(function_object("destructed",obj)) {
		obj->destructed(1);
	    }

	    return;
	}

	objname = object_name(obj);

	/* Not a clone */
	if(dest_issues[objname] || dest_issues[index]) {
	    if(dest_issues[objname])
		LOGD->log("Name '" + (objname)
		  + "' already in dest_issues.",
		  "objectd");
	    if(dest_issues[index])
		LOGD->log("Index " + index + " already in dest_issues.",
		  "objectd");

	    LOGD->log("Object is already in dest_issues!", "objectd");
	}

	/* Put into dest_issues */
	dest_issues[objname] = index;
	dest_issues[index] = index;

	/* Mark issue destroyed */
	if(issue)
	    issue->destruct();

	if(function_object("destructed", obj)) {
	    obj->destructed(0);
	}

    }
}

/* Inheritable object is about to be destructed */
void destruct_lib(string owner, string path)
{
    if(previous_program() == DRIVER) {
	object issue;
	int    index;

	index = status(path)[O_INDEX];
	issue = obj_issues->index(index);

	LOGD->log("destruct_lib: " + path + " (" + index + ")",
	  "objectd");

	all_libs[path] = nil;

	/* If the Obj is not registered, that's fine unless we've
	   aggro_recompile'd and set stronger checking on. */
	if(!issue && aggro_recompile > 1) {
	    LOGD->log("Can't get issue for lib "
	      + path + " in destruct_lib!", "objectd");
	}
	if(!issue) return;

	if(dest_issues[path] || dest_issues[index]) {
	    LOGD->log("Lib is already in dest_issues!", "objectd");
	}

	dest_issues[path] = index;
	dest_issues[index] = index;

	if(issue)
	    issue->destruct();
    }
}

/* Last ref to specific issue removed */
void remove_program(string owner, string path, int timestamp, int index)
{
    if(previous_program() == DRIVER) {
	object issue, cur;
	mixed* status;

	/*LOGD->log("remove: " + path + ", issue #" + index, "objectd");*/

	/* Get current version */
	cur = find_object(path);
	status = status(cur ? cur : path);
	if(status) {
	    cur = obj_issues->index(status[O_INDEX]);
	} else {
	    if(dest_issues[path]) {
		cur = obj_issues->index(dest_issues[path]);
		if(!cur)
		    LOGD->log("No current instance of " + path + "!", "objectd");
	    }
	}

	if(!cur) {
	    LOGD->log("Removing issueless stray " + path, "objectd");
	}

	issue = obj_issues->index(index);
	if(issue) {
	    if(issue->destroyed()) {
		dest_issues[index] = nil;
		dest_issues[path] = nil;
	    }
	} else if(dest_issues[path] || dest_issues[index]) {
	    LOGD->log("Issue in dest_issues but not obj_issues!", "objectd");
	}

	if(issue) {
	    unregister_inherit_data(issue);

	    /* For libraries only, clear child data */
	    if(function_object("get_children", issue))
		clear_child_data(issue);
	} else if(aggro_recompile > 1) {
	    /* This is either a mistake in ObjectD's tracking or an issue
	       of a library from ObjectD's initialization. */
	    if(untracked_libs && untracked_libs[path]) {
		untracked_libs[path]--;
		if(untracked_libs[path] <= 0) {
		    untracked_libs[path] = nil;
		}
	    } else
		LOGD->log("Removing unregistered issue of " + path, "objectd");
	}

	obj_issues->set_index(index, nil);

	/* If no current issue is to be found, this no longer
	   belongs in all_libs */
	if(!status(path)) {
	    all_libs[path] = nil;
	}

    }
}


/* Object (possibly a lib) is about to be compiled */
void compiling(string path)
{
    if(previous_program() == DRIVER) {
	object obj, issue;
	int    recomp;

	LOGD->log("compiling: " + path, "objectd");

	/* Set up entry to keep track of objects being compiled */
	last_file = path;
	comp_dep = ({ });

	/* Call upgrading() func to let object know it's being compiled */
	obj = find_object(path);
	if(obj && function_object("upgrading", obj)) {
	    catch {
		obj->upgrading();
	    } : {
		LOGD->log("Error in " + object_name(obj) + "->upgrading()",
		  "objectd");
	    }
	}
    }
}

/* 'path' about to be included */
mixed include_file(string compiled, string from, string path)
{
    if(previous_program() == DRIVER && aggro_recompile > 0) {
	string trimmed_from;

	/*LOGD->log("include " + path + " from " + from,
	  "objectd");*/

	if(path != "AUTO" && path != "/include/AUTO" && path != nil
	  && path != ""
	  && sscanf(path, "%*s\.h") == 0)
	    LOGD->log("Including non-header file '" + path + "'", "objectd");

	comp_dep += ({ path });
    }


    return path;
}

/* An attempt to compile the given object (including libs) has failed */
void compile_failed(string owner, string path)
{
    if(previous_program() == DRIVER) {
	comp_dep = nil;

	LOGD->log("compile_failed: " + path, "objectd");
    }
}

/* Non-System files can inherit from a nonstandard AUTO object if the
   ObjectD returns one.  This is where that happens. */
/* We check the auto_objects mapping for directories that have been
   registered for special treatement. */
string path_special(string compiled)
{
    if(path_special_object) {
	string tmp;
	tmp = path_special_object->path_special(compiled);
	if(!tmp)
	    tmp = "";

	return tmp;
    }

    return "";
}

/* Return true if path is not a legal first arg for call_other */
int forbid_call(string path)
{
    if(previous_program() == DRIVER) {
    }
    return 0;
}

/* Return true to forbid 'from' from inheriting 'path', privately or no
   according to 'priv' */
int forbid_inherit(string from, string path, int priv)
{
    if(previous_program() == DRIVER) {
	/*LOGD->log("forbid_inherit: " + path + " from " + from,
	  "objectd");*/

	/* If we *did* actually forbid something, that would be logged with
	   something other than LOG_ULTRA_VERBOSE... */
    }
    return 0;
}

/** @todo use touch to facilitate upgrades
 * NAME:  touch()
 * DESCRIPTION:  An object which has been marked by call_touch() is about to have the
 * given function called in it.  A non-zero return value indicates that the
 * object's "untouched" status should be preserved through the following
 * call.
 */
int touch(object obj, string func){
    /* upgrade calls should be made here, that would be things in order to update old objects to new objects */

    return 0;
}


/*** API Funcs called by the system at large ***/

/* Security is handled here by allowing the setup only once, and the
   fact that the operation gives information only to this object. */
void do_initial_obj_setup(void) {
    string* owners;

    /* For object iteration: */
    object  index, first;
    object* obj_arr;

    if(!SYSTEM())
	return;

    /* This just makes sure that do_initial_obj_setup() is only called once */
    if(setup_done) return;
    setup_done = 1;

    owners = rsrc::query_owners();

    recompile_every_object(owners);
    count_clones(owners);
}

/* Gives a list of destroyed object names */
string destroyed_obj_list(void) {
    mixed*  keys;
    int     ctr, idx;
    object  issue;
    string  ret;

    if(!SYSTEM())
	return nil;

    ret = "Objects:\n";
    keys = map_indices(dest_issues);
    for(ctr = 0; ctr < sizeof(keys); ctr++) {
	if(typeof(keys[ctr]) == T_INT) {
	    /* Index is num, not name */
	    continue;
	}

	ret += "* ";
	idx = dest_issues[keys[ctr]];
	issue = obj_issues->index(idx);
	if(issue) {
	    ret += issue->get_name();
	} else {
	    ret += "(nil)";
	}
	ret += " (" + idx + ")" + "\n";
    }

    return ret;
}

/* This is secure because it exports only a string.  You can find out
   how many clones an object has, but you can't get their object
   pointers. */
string report_on_object(string spec) {
    object issue;
    string ret;
    int    ctr, lib;
    mixed* arr, status;
    mixed  index;

    if(!SYSTEM())
	return nil;

    if(sscanf(spec, "#%d", index) || sscanf(spec, "%d", index)) {
	issue = obj_issues->index(index);

	if(!issue && dest_issues[index]) {
	    LOGD->log("Issue is in dest_issues but not obj_issues!",
	      "objectd");
	}

	if(!issue)
	    return "Issue #" + index + " isn't tracked by objectd\n";

    } else {
	status = status(spec);
	if(status) {
	    index = status[O_INDEX];
	    issue = obj_issues->index(index);
	    if(!issue && !find_object(spec))
		return "DGD tracks " + spec + " (#" + index
		+ ") but objectd does not.\n";
	}
	if(find_object(spec)) {
	    index = status(find_object(spec))[O_INDEX];
	    issue = obj_issues->index(index);
	    if(!issue)
		return "DGD recognizes obj " + spec + " but objectd does not.\n";
	} else if(!issue) {
	    if(dest_issues[spec]) {
		issue = obj_issues->index(dest_issues[spec]);
	    } else
		return "DGD doesn't recognize object " + spec + "\n";
	}
    }

    ret = "";

    if(issue->destroyed() && dest_issues[issue->get_index()])
	ret += "Destroyed ";
    else if (issue->destroyed() || dest_issues[issue->get_index()]) {
	if(dest_issues[issue->get_index()]) {
	    ret += "(In Dest array)\n";
	} else {
	    ret += "(Marked destroyed)\n";
	}
	ret += "Incorrectly destroyed ";
    }

    if(object_name(issue) == (LIB_LWO + "#-1")) {
	ret += "Inheritable\n";
	lib = 1;
    } else if(object_name(issue) == (CLONABLE_LWO + "#-1")) {
	ret += "Clonable\n";
	ret += "  " + issue->get_num_clones() + " clones exist\n";
    } else {
	return "Internal error!";
    }

    if(typeof(issue->get_name()) != T_STRING) {
	error("Issue name isn't a string...");
    }
    ret += "Name: " + issue->get_name() + "\n";
    ret += "Index: " + issue->get_index() + "\n";
    ret += "Comp_time: " + ctime(issue->get_comp_time()) + "\n";
    if(issue->get_prev() != -1) {
	ret += "Previous issue: " + issue->get_prev() + "\n";
    } else {
	ret += "No previous issue\n";
    }

    ret += "Inherits from: ";
    arr = issue->get_parents();
    if(!arr) arr = ({ });
    for(ctr = 0; ctr < sizeof(arr); ctr++) {
	index = arr[ctr];
	if(typeof(index) == T_STRING)
	    ret += index + " ";
	else
	    ret += "#" + index + " ";
    }
    ret += "\n";

    if(lib) {
	ret += "Its children are: ";
	arr = issue->get_children();
	if(!arr) arr = ({ });
	for(ctr = 0; ctr < sizeof(arr); ctr++) {
	    index = arr[ctr];
	    ret += "#" + index + " ";
	}
	ret += "\n";
    }

    ret += "Also depends on: ";
    arr = issue->get_dependencies();
    if(!arr) arr = ({ });
    for(ctr = 0; ctr < sizeof(arr); ctr++) {
	index = arr[ctr];
	if(typeof(index) == T_STRING)
	    ret += index + " ";
	else
	    error("Non-string dependency!");
    }
    ret += "\n";

    return ret;
}

/*
  Suspend_system suspends network input, new logins and callouts
  except in this object.  (idea stolen from Geir Harald Hansen's
  ObjectD).  This will need to be copied to any and every object
  that suspends callouts -- the RSRCD checks previous_object()
  to find out who *isn't* suspended.  TelnetD only suspends
  new incoming network activity.
*/
private void suspend_system() {
    if(SYSTEM()) {
	RSRCD->suspend_callouts();
	TEL_MANAGER->suspend_input(0);  /* 0 means "not shutdown" */
    } else
	error("Only privileged code can call suspend_system!");
}

/*
  Releases everything that suspend_system suspends.
*/
private void release_system() {
    if(SYSTEM()) {
	RSRCD->release_callouts();
	TEL_MANAGER->release_input();
    } else
	error("Only privileged code can call release_system!");
}


/* Returns a status report on an object (passed in directly or
   by path, object name or issue number).  The report is in
   the following format (offsets 0 to 6):

   ({ issue number (T_INT),
      object name (T_STRING),
      parent array (T_ARRAY) or nil,
      child array (T_ARRAY) or nil,
      num_clones (T_INT) or nil,
      previous issue number (T_INT) or nil,
      object destroyed or not (0 or 1, T_INT)
    })
*/
mixed* od_status(mixed obj_spec) {
    object issue;
    int    index;

    if(!SYSTEM())
	return nil;

    if(typeof(obj_spec) == T_INT) {
	index = obj_spec;
	issue = obj_issues->index(obj_spec);
    } else if(typeof(obj_spec) == T_OBJECT) {
	index = status(obj_spec)[O_INDEX];
	issue = obj_issues->index(obj_spec);
    } else if(typeof(obj_spec) == T_STRING) {
	if(find_object(obj_spec)) {
	    index = status(find_object(obj_spec))[O_INDEX];
	} else {
	    if(status(obj_spec)) {
		index = status(obj_spec)[O_INDEX];
	    } else {
		return nil;
	    }
	}
	issue = obj_issues->index(index);
    }

    if(!issue) return nil;

    return ({ index, issue->get_name(), issue->get_parents(),
      issue->get_children(), issue->get_num_clones(),
      issue->get_prev() != -1 ? issue->get_prev() : nil,
      issue->destroyed() });
}

/* Recompiles all objects other than DRIVER.  Output is sent to object
  output using the "message" function.  The user object happens to work
  well with this :-) */
void recompile_auto_object(object output) {
    int    ctr, ctr2;
    mixed* keys, *didnt_dest;

    LOGD->log("Demanding auto recompile "+previous_program(), "objectd");
    if(!SYSTEM() && !KERNEL() && previous_program() != "/usr/hymael/_code")
	return;

    /* This will always be logged at this level */
    LOGD->log("Doing full rebuild...", "objectd");

    if(!SYSTEM() && !KERNEL() && previous_program() != "/usr/hymael/_code")
	error("Can't call recompile_auto_object unprivileged!");

    /* Destruct all libs... */
    keys = map_indices(all_libs);
    didnt_dest = ({ });
    ctr2 = 0;
    for(ctr = 0; ctr < sizeof(keys); ctr++) {
	if(status(keys[ctr])) {
	    ctr2++;
	    destruct_object(keys[ctr]);
	} else {
	    didnt_dest += ({ keys[ctr] });
	}
    }
    ctr2++;
    destruct_object(AUTO);

    if(ctr2 != sizeof(keys) + 1) {
	/* In this case, some elements of all_libs are already destructed,
	   which violates our assumptions.  Of course, the rebuild will
	   probably fix it. */
	LOGD->log("Couldn't destruct " + sizeof(didnt_dest) + "/"
	  + (sizeof(keys) + 1) + " libs: "
	  + implode(didnt_dest, ", "), "objectd");
    }

    /* Recompile all clonables */
    recompile_every_clonable(rsrc::query_owners());
}


void set_path_special(object new_manager) {
    if(SYSTEM()) {
	path_special_object = new_manager;
    } else {
	error("Only System objects can set the path_special object!");
    }
}
