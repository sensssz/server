#ifndef ITEM_JSONFUNC_INCLUDED
#define ITEM_JSONFUNC_INCLUDED

/* Copyright (c) 2016, MariaDB

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA */


/* This file defines all XML functions */


#ifdef USE_PRAGMA_INTERFACE
#pragma interface			/* gcc class implementation */
#endif

#include "item_cmpfunc.h"      // Item_bool_func
#include "item_strfunc.h"      // Item_str_func
#include "sql_json.h"


class json_path_with_flags : public json_path
{
public:
  bool constant;
  bool parsed;
  json_path_step *cur_step;
  void set_constant_flag(bool s_constant)
  {
    constant= s_constant;
    parsed= FALSE;
  }
};


class Item_func_json_valid: public Item_int_func
{
protected:
  String tmp_value;

public:
  Item_func_json_valid(THD *thd, Item *json) : Item_int_func(thd, json) {}
  longlong val_int();
  const char *func_name() const { return "json_valid"; }
  void fix_length_and_dec()
  {
    Item_int_func::fix_length_and_dec();
    maybe_null= 1;
  }
};


class Item_func_json_value: public Item_str_func
{
protected:
  json_path_with_flags path;
  String tmp_js, tmp_path;

public:
  Item_func_json_value(THD *thd, Item *js, Item *path):
    Item_str_func(thd, js, path) {}
  const char *func_name() const { return "json_value"; }
  void fix_length_and_dec();
  String *val_str(String *);
};


class Item_func_json_extract: public Item_str_func
{
protected:
  String tmp_js;
  json_path_with_flags *paths;
  String **tmp_paths;

public:
  Item_func_json_extract(THD *thd, List<Item> &list):
    Item_str_func(thd, list) {}
  const char *func_name() const { return "json_extract"; }
  bool fix_fields(THD *thd, Item **ref);
  void fix_length_and_dec();
  String *val_str(String *);
  longlong val_int();
};


class Item_func_json_contains_path: public Item_int_func
{
protected:
  String tmp_js;
  json_path_with_flags *paths;
  String **tmp_paths;
  bool mode_one;
  bool ooa_constant, ooa_parsed;

public:
  Item_func_json_contains_path(THD *thd, List<Item> &list):
    Item_int_func(thd, list) {}
  const char *func_name() const { return "json_contains_path"; }
  bool fix_fields(THD *thd, Item **ref);
  void fix_length_and_dec();
  longlong val_int();
};


class Item_func_json_array_append: public Item_str_func
{
protected:
  String tmp_js;
  String tmp_val;
  json_path_with_flags *paths;
  String **tmp_paths;
public:
  Item_func_json_array_append(THD *thd, List<Item> &list):
    Item_str_func(thd, list) {}
  bool fix_fields(THD *thd, Item **ref);
  void fix_length_and_dec();
  String *val_str(String *);
  const char *func_name() const { return "json_array_append"; }
};


#endif /* ITEM_JSONFUNC_INCLUDED */
