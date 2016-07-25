#ifndef SQL_JSON_INCLUDED
#define SQL_JSON_INCLUDED

#include <m_ctype.h>

static const int json_depth_limit= 32;

enum json_errors {
  JE_BAD= -1,       /* Invalid character - cannot read the string. */
  JE_IMP= -2,       /* This character disallowed in JSON. */
  JE_EOS= -3,       /* Unexpected end of string. */
  JE_UEX= -4,       /* Syntax error - unexpected character. */
  JE_IMS= -5,       /* This character disallowed in string constant. */
  JE_ESCAPING= -6,  /* Wrong escaping. */
  JE_TOODEEP= -7,   /* The limit on the depth was overrun. */
};


class json_string
{
public:
  const uchar *c_str;
  const uchar *str_end;
  my_wc_t c_next;
  int error;

  CHARSET_INFO *cs;
  my_charset_conv_mb_wc wc;
  int get_next_char()
  {
    return wc(cs, &c_next, c_str, str_end);
  }
  my_wc_t next_chr() const { return c_next; }
  bool eos() const { return c_str >= str_end; }
  int handle_esc();
  void skip_s_getchar(int &t_next, int &c_len);
  void set_cs(CHARSET_INFO *i_cs);
  void set_str(const uchar *str, const uchar *end)
  {
    c_str= str;
    str_end= end;
  }
  void setup(CHARSET_INFO *i_cs, const uchar *str, const uchar *end)
  {
    set_cs(i_cs);
    set_str(str, end);
  }
  int read_str_chr();
};


struct json_path_step
{
  enum types { KEY=0, ARRAY=1 };

  types type;
  int wild;
  const uchar *key;
  const uchar *key_end;
  uint n_item;
};


class json_path : public json_string
{
public:
  json_path_step steps[json_depth_limit];
  json_path_step *last_step;

  bool mode_strict;
  int setup(CHARSET_INFO *i_cs, const uchar *str, const uchar *end);
};


class json_engine : public json_string
{
public:
  enum states {
    VALUE, /* value met        */
    DONE,  /* ok to finish     */
    KEY,   /* key met          */
    OB_B,  /* object           */
    OB_E,  /* object ended     */
    OB_C,  /* object continues */
    AR_B,  /* array            */
    AR_E,  /* array ended      */
    AR_C,  /* array continues  */
    READ_VALUE, /* value is beeing read */
    NR_STATES
  };

  enum value_types
  { OBJECT=0, ARRAY=1, STRING, NUMBER, V_TRUE, V_FALSE, V_NULL };

  int sav_c_len;
  value_types value_type;
  const uchar *value;
  const uchar *value_begin;
  const uchar *value_end;
  int value_len;

  int stack[json_depth_limit];
  int *stack_p;

  int state;

  int scan_start(CHARSET_INFO *i_cs, const uchar *str, const uchar *end);
  int scan_next();

  int read_keyname_chr();

  int read_value();
  int skip_key();
  int skip_level();
  int skip_array_item() { return skip_key(); }
  bool value_scalar() const { return value_type > ARRAY; }
};

#endif /* SQL_JSON_INCLUDED */

