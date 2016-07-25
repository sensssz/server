/* Copyright (c) 2016, Monty Program Ab.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA */

#ifdef __GNUC__
#pragma implementation
#endif

#include <my_global.h>
#include "sql_priv.h"
#include "sql_class.h"
#include "item.h"


static bool eq_ascii_string(const CHARSET_INFO *cs,
                            const char *ascii,
                            const char *s,  uint32 s_len)
{
  const char *s_end= s + s_len;

  while (*ascii && s < s_end)
  {
    my_wc_t wc;
    int wc_len;

    wc_len= cs->cset->mb_wc(cs, &wc, (uchar *) s, (uchar *) s_end);
    if (wc_len <= 0 || (wc | 0x20) != (my_wc_t) *ascii)
      return 0;

    ascii++;
    s+= wc_len;
  }

  return *ascii == 0 && s >= s_end;
}


longlong Item_func_json_valid::val_int()
{
  String *js= args[0]->val_str(&tmp_value);
  json_engine je;

  if ((null_value= args[0]->null_value) || js == NULL)
    return 0;

  je.scan_start(js->charset(), (const uchar *) js->ptr(),
                (const uchar *) js->ptr()+js->length());

  while (je.scan_next() == 0) {}

  return je.error == 0;
}


void Item_func_json_value::fix_length_and_dec()
{
  collation.set(args[0]->collation);
  max_length= args[0]->max_length;
  path.constant= args[1]->const_item();
  path.parsed= FALSE;
}


static int handle_match(json_engine *je, json_path_with_flags *p, uint *n_arrays)
{
  DBUG_ASSERT(p->cur_step < p->last_step);

  if (je->read_value())
    return 1;

  if (je->value_scalar())
    return 0;

  p->cur_step++;
  n_arrays[p->cur_step - p->steps]= 0;

  if ((int) je->value_type != (int) p->cur_step->type)
  {
    p->cur_step--;
    return je->skip_level();
  }

  return 0;
}


static int json_key_matches(json_engine *je, json_string *k)
{
  while (je->read_keyname_chr() == 0)
  {
    if (k->read_str_chr() ||
        je->next_chr() != k->next_chr())
      return 0;
  }

  if (k->read_str_chr())
    return 1;

  return 0;
}


static int find_value(json_engine *je, String *js, json_path_with_flags *p)
{
  json_string kn;
  uint n_arrays[json_depth_limit];

  kn.set_cs(p->cs);

  do
  {
    json_path_step *cur_step= p->cur_step;
    switch (je->state)
    {
    case json_engine::KEY:
      DBUG_ASSERT(cur_step->type == json_path_step::KEY);
      if (!cur_step->wild)
      {
        kn.set_str(cur_step->key, cur_step->key_end);
        if (!json_key_matches(je, &kn))
        {
          if (je->skip_key())
            goto exit;
          continue;
        }
      }
      if (cur_step == p->last_step ||
          handle_match(je, p, n_arrays))
        goto exit;
      break;
    case json_engine::VALUE:
      DBUG_ASSERT(cur_step->type == json_path_step::ARRAY);
      if (cur_step->wild ||
          cur_step->n_item == n_arrays[cur_step - p->steps])
      {
        /* Array item matches. */
        if (cur_step == p->last_step ||
            handle_match(je, p, n_arrays))
          goto exit;
      }
      else
      {
        je->skip_array_item();
        n_arrays[cur_step - p->steps]++;
      }
      break;
    case json_engine::OB_E:
    case json_engine::AR_E:
      p->cur_step--;
      break;
    default:
      DBUG_ASSERT(0);
      break;
    };
  } while (je->scan_next() == 0);

  /* No luck. */
  return 1;

exit:
  return je->error;
}


String *Item_func_json_value::val_str(String *str)
{
  json_engine je;
  String *js= args[0]->val_str(&tmp_js);

  if (!path.constant || !path.parsed)
  {
    String *s_p= args[1]->val_str(&tmp_path);
    if (s_p &&
        path.setup(s_p->charset(), (const uchar *) s_p->ptr(),
          (const uchar *) s_p->ptr() + s_p->length()))
      goto error;
    path.parsed= TRUE;
  }

  if ((null_value= args[0]->null_value || args[1]->null_value))
    return NULL;

  je.scan_start(js->charset(),(const uchar *) js->ptr(),
                (const uchar *) js->ptr() + js->length());

  path.cur_step= path.steps;
continue_search:
  if (find_value(&je, js, &path))
  {
    if (je.error)
      goto error;

    null_value= 1;
    return 0;
  }

  if (je.read_value())
    goto error;

  if (!je.value_scalar())
  {
    /* We only look for scalar values! */
    if (je.skip_level() || je.scan_next())
      goto error;
    goto continue_search;
  }

  str->set((const char *) je.value, je.value_len, je.cs);
  return str;

error:
  null_value= 1;
  return 0;
}


static int alloc_tmp_paths(THD *thd, uint n_paths,
                           json_path_with_flags **paths,String ***tmp_paths)
{
  uint n;
  *paths= (json_path_with_flags *) alloc_root(thd->mem_root,
                                     sizeof(json_path_with_flags) * n_paths);
  *tmp_paths= (String **) alloc_root(thd->mem_root, sizeof(String *) * n_paths);
  if (*paths == 0 || *tmp_paths == 0)
    return 1;

  for (n=0; n<n_paths; n++)
  {
    if (((*tmp_paths)[n]= new(thd->mem_root) String) == 0)
      return 1;
  }
  return 0;
}


static void mark_constant_paths(json_path_with_flags *p,
                                Item** args, uint n_args)
{
  uint n;
  for (n= 0; n < n_args; n++)
    p[n].set_constant_flag(args[n]->const_item());
}


bool Item_func_json_extract::fix_fields(THD *thd, Item **ref)
{
  return alloc_tmp_paths(thd, arg_count-1, &paths, &tmp_paths) ||
         Item_str_func::fix_fields(thd, ref);
}


void Item_func_json_extract::fix_length_and_dec()
{
  collation.set(args[0]->collation);
  max_length= args[0]->max_length * (arg_count - 1);

  mark_constant_paths(paths, args+1, arg_count-1);
}


String *Item_func_json_extract::val_str(String *str)
{
  String *js= args[0]->val_str(&tmp_js);
  json_engine je;
  bool multiple_values_found= FALSE;
  const uchar *value;
  const char *first_value= NULL, *first_p_value;
  uint n_arg, v_len, first_len, first_p_len;

  if ((null_value= args[0]->null_value))
    return 0;

  str->set_charset(js->charset());
  str->length(0);

  for (n_arg=1; n_arg < arg_count; n_arg++)
  {
    json_path_with_flags *c_path= paths + n_arg - 1;
    if (!c_path->constant || !c_path->parsed)
    {
      String *s_p= args[n_arg]->val_str(tmp_paths[n_arg-1]);
      if (s_p &&
          c_path->setup(s_p->charset(), (const uchar *) s_p->ptr(),
                        (const uchar *) s_p->ptr() + s_p->length()))
        goto error;
      c_path->parsed= TRUE;
    }

    je.scan_start(js->charset(),(const uchar *) js->ptr(),
        (const uchar *) js->ptr() + js->length());

    c_path->cur_step= c_path->steps;

    if (find_value(&je, js, c_path))
    {
      /* Path wasn't found. */
      if (je.error)
        goto error;

      continue;
    }

    if (je.read_value())
      goto error;

    value= je.value_begin;
    if (je.value_scalar())
      v_len= je.value_end - value;
    else
    {
      if (je.skip_level())
        goto error;
      v_len= je.c_str - value;
    }

    if (!multiple_values_found)
    {
      if (first_value == NULL)
      {
        /*
          Just remember the first value as we don't know yet
          if we need to create an array out of it or not.
        */
        first_value= (const char *) value;
        first_len= v_len;
        /*
         We need this as we have to preserve quotes around string
          constants if we use the value to create an array. Otherwise
          we get the value without the quotes.
        */
        first_p_value= (const char *) je.value;
        first_p_len= je.value_len;
        continue;
      }
      else
      {
        multiple_values_found= TRUE; /* We have to make an JSON array. */
        if ( str->append("[") ||
            str->append(first_value, first_len))
          goto error; /* Out of memory. */
      }

    }
    if (str->append(", ", 2) ||
        str->append((const char *) value, v_len))
      goto error; /* Out of memory. */
  }

  if (first_value == NULL)
  {
    /* Nothing was found. */
    null_value= 1;
    return 0;
  }

  if (multiple_values_found ?
        str->append("]") :
        str->append(first_p_value, first_p_len))
    goto error; /* Out of memory. */

  return str;

error:
  /* TODO: launch error messages. */
  null_value= 1;
  return 0;
}


longlong Item_func_json_extract::val_int()
{
  String *js= args[0]->val_str(&tmp_js);
  json_engine je;
  uint n_arg;

  if ((null_value= args[0]->null_value))
    return 0;

  for (n_arg=1; n_arg < arg_count; n_arg++)
  {
    json_path_with_flags *c_path= paths + n_arg - 1;
    if (!c_path->constant || !c_path->parsed)
    {
      String *s_p= args[n_arg]->val_str(tmp_paths[n_arg-1]);
      if (s_p &&
          c_path->setup(s_p->charset(), (const uchar *) s_p->ptr(),
                        (const uchar *) s_p->ptr() + s_p->length()))
        goto error;
      c_path->parsed= TRUE;
    }

    je.scan_start(js->charset(),(const uchar *) js->ptr(),
        (const uchar *) js->ptr() + js->length());

    c_path->cur_step= c_path->steps;

    if (find_value(&je, js, c_path))
    {
      /* Path wasn't found. */
      if (je.error)
        goto error;

      continue;
    }

    if (je.read_value())
      goto error;

    if (je.value_scalar())
    {
      int err;
      char *v_end= (char *) je.value_end;
      return (je.cs->cset->strtoll10)(je.cs, (const char *) je.value_begin,
                                      &v_end, &err);
    }
    else
      break;
  }

  /* Nothing was found. */
  null_value= 1;
  return 0;

error:
  /* TODO: launch error messages. */
  null_value= 1;
  return 0;
}


bool Item_func_json_contains_path::fix_fields(THD *thd, Item **ref)
{
  return alloc_tmp_paths(thd, arg_count-2, &paths, &tmp_paths) ||
         Item_int_func::fix_fields(thd, ref);
}


void Item_func_json_contains_path::fix_length_and_dec()
{
  ooa_constant= args[1]->const_item();
  ooa_parsed= FALSE;
  mark_constant_paths(paths, args+2, arg_count-2);
  Item_int_func::fix_length_and_dec();
}


longlong Item_func_json_contains_path::val_int()
{
  String *js= args[0]->val_str(&tmp_js);
  json_engine je;
  uint n_arg;
  longlong result;

  if ((null_value= args[0]->null_value))
    return 0;

  if (!ooa_constant || !ooa_parsed)
  {
    char buff[20];
    String *res, tmp(buff, sizeof(buff), &my_charset_bin);
    res= args[1]->val_str(&tmp);
    mode_one=eq_ascii_string(res->charset(), "one",
                             res->ptr(), res->length());
    if (!mode_one)
    {
      if (!eq_ascii_string(res->charset(), "all", res->ptr(), res->length()))
        goto error;
    }
    ooa_parsed= TRUE;
  }

  result= !mode_one;
  for (n_arg=2; n_arg < arg_count; n_arg++)
  {
    json_path_with_flags *c_path= paths + n_arg - 2;
    if (!c_path->constant || !c_path->parsed)
    {
      String *s_p= args[n_arg]->val_str(tmp_paths[n_arg-2]);
      if (s_p &&
          c_path->setup(s_p->charset(), (const uchar *) s_p->ptr(),
                        (const uchar *) s_p->ptr() + s_p->length()))
        goto error;
      c_path->parsed= TRUE;
    }

    je.scan_start(js->charset(),(const uchar *) js->ptr(),
        (const uchar *) js->ptr() + js->length());

    c_path->cur_step= c_path->steps;
    if (find_value(&je, js, c_path))
    {
      /* Path wasn't found. */
      if (je.error)
        goto error;

      if (!mode_one)
      {
        result= 0;
        break;
      }
    }
    else if (mode_one)
    {
      result= 1;
      break;
    }
  }


  return result;

error:
  null_value= 1;
  return 0;
}


bool Item_func_json_array_append::fix_fields(THD *thd, Item **ref)
{
  return alloc_tmp_paths(thd, arg_count/2, &paths, &tmp_paths) ||
         Item_str_func::fix_fields(thd, ref);
}


void Item_func_json_array_append::fix_length_and_dec()
{
  uint n_arg;
  ulonglong char_length;

  collation.set(args[0]->collation);
  char_length= args[0]->max_char_length();

  for (n_arg= 1; n_arg < arg_count; n_arg+= 2)
  {
    paths[n_arg-1].set_constant_flag(args[n_arg]->const_item());
    char_length+= args[n_arg+1]->max_char_length() + 4;
  }

  fix_char_length_ulonglong(char_length);
}


static int append_json(String *str, Item *item, String *tmp_val)
{
  if (item->is_bool_type())
  {
    longlong v_int= item->val_int();
    if (item->null_value)
      goto append_null;

    const char *t_f;
    t_f= v_int ? "true" : "false";
    return str->append(t_f, strlen(t_f));
  }
  {
    String *sv= item->val_str(tmp_val);
    if (item->null_value)
      goto append_null;
    return str->append(*sv);
  }

append_null:
  return str->append("null", 4);
}


String *Item_func_json_array_append::val_str(String *str)
{
  json_engine je;
  String *js= args[0]->val_str(&tmp_js);
  uint n_arg, n_path, str_rest_len;
  const uchar *ar_end;

  DBUG_ASSERT(fixed == 1);

  if ((null_value= args[0]->null_value))
    return 0;

  for (n_arg=1, n_path=0; n_arg < arg_count; n_arg+=2, n_path++)
  {
    json_path_with_flags *c_path= paths + n_path;
    if (!c_path->constant || !c_path->parsed)
    {
      String *s_p= args[n_arg]->val_str(tmp_paths[n_path]);
      if (s_p &&
          c_path->setup(s_p->charset(), (const uchar *) s_p->ptr(),
                        (const uchar *) s_p->ptr() + s_p->length()))
        goto error;
      c_path->parsed= TRUE;
    }
    if (args[n_arg]->null_value)
    {
      null_value= 1;
      return 0;
    }

    je.scan_start(js->charset(),(const uchar *) js->ptr(),
                  (const uchar *) js->ptr() + js->length());
    c_path->cur_step= c_path->steps;

    if (find_value(&je, js, c_path))
    {
      if (je.error)
        goto error;
      null_value= 1;
      return 0;
    }

    if (je.read_value())
      goto error;

    if (je.value_type != json_engine::ARRAY)
    {
      /* Must be an array. */
      goto error;
    }

    if (je.skip_level())
      goto error;

    str->length(0);
    str->set_charset(js->charset());
    if (str->reserve(js->length() + 8, 512))
      goto error; /* Out of memory. */
    ar_end= je.c_str - je.sav_c_len;
    str_rest_len= js->length() - (ar_end - (const uchar *) js->ptr());
    str->q_append(js->ptr(), ar_end-(const uchar *) js->ptr());
    str->append(", ", 2);
    if (append_json(str, args[n_arg+1], &tmp_val))
      goto error; /* Out of memory. */

    if (str->reserve(str->length() + str_rest_len, 512))
      goto error; /* Out of memory. */
    str->q_append((const char *) ar_end, str_rest_len);
  }

  return str;

error:
  null_value= 1;
  return 0;
}

