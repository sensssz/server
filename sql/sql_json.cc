#include <my_global.h>

#include "sql_json.h"


/* Copied from ctype-ucs2.c. */

/*
  D800..DB7F - Non-provate surrogate high (896 pages)
  DB80..DBFF - Private surrogate high     (128 pages)
  DC00..DFFF - Surrogate low              (1024 codes in a page)
*/
#define MY_UTF16_SURROGATE_HIGH_FIRST 0xD800
#define MY_UTF16_SURROGATE_HIGH_LAST  0xDBFF
#define MY_UTF16_SURROGATE_LOW_FIRST  0xDC00
#define MY_UTF16_SURROGATE_LOW_LAST   0xDFFF

#define MY_UTF16_HIGH_HEAD(x)      ((((uchar) (x)) & 0xFC) == 0xD8)
#define MY_UTF16_LOW_HEAD(x)       ((((uchar) (x)) & 0xFC) == 0xDC)
/* Test if a byte is a leading byte of a high or low surrogate head: */
#define MY_UTF16_SURROGATE_HEAD(x) ((((uchar) (x)) & 0xF8) == 0xD8)
/* Test if a Unicode code point is a high or low surrogate head */
#define MY_UTF16_SURROGATE(x)      (((x) & 0xF800) == 0xD800)

#define MY_UTF16_WC2(a, b)         ((a << 8) + b)

/*
  a= 110110??  (<< 18)
  b= ????????  (<< 10)
  c= 110111??  (<<  8)
  d= ????????  (<<  0)
*/
#define MY_UTF16_WC4(a, b, c, d) (((a & 3) << 18) + (b << 10) + \
                                  ((c & 3) << 8) + d + 0x10000)

int
my_utf16_uni(CHARSET_INFO *cs __attribute__((unused)),
             my_wc_t *pwc, const uchar *s, const uchar *e)
{
  if (s + 2 > e)
    return MY_CS_TOOSMALL2;
  
  /*
    High bytes: 0xD[89AB] = B'110110??'
    Low bytes:  0xD[CDEF] = B'110111??'
    Surrogate mask:  0xFC = B'11111100'
  */

  if (MY_UTF16_HIGH_HEAD(*s)) /* Surrogate head */
  {
    if (s + 4 > e)
      return MY_CS_TOOSMALL4;

    if (!MY_UTF16_LOW_HEAD(s[2]))  /* Broken surrigate pair */
      return MY_CS_ILSEQ;

    *pwc= MY_UTF16_WC4(s[0], s[1], s[2], s[3]);
    return 4;
  }

  if (MY_UTF16_LOW_HEAD(*s)) /* Low surrogate part without high part */
    return MY_CS_ILSEQ;

  *pwc= MY_UTF16_WC2(s[0], s[1]);
  return 2;
}

/* End of ctype-ucs2.c code. */

enum json_c_classes {
  C_EOS,    /* end of string */
  C_LCURB,  /* {  */
  C_RCURB,  /* } */
  C_LSQRB,  /* [ */
  C_RSQRB,  /* ] */
  C_COLON,  /* : */
  C_COMMA,  /* , */
  C_QUOTE,  /* " */
  C_DIGIT,  /* -0123456789 */
  C_LOW_F,  /* f */
  C_LOW_N,  /* n */
  C_LOW_T,  /* t */
  C_ETC,    /* everything else */
  C_ERR,    /* character disallowed in JSON*/
  C_BAD,    /* invalid character */
  NR_C_CLASSES,
  C_SPACE   /* space */
};


/*
  This array maps first 128 Unicode Code Points into classes.
  The remaining Unicode characters should be mapped to C_ETC.
*/

static int json_chr_map[128] = {
  C_ERR,   C_ERR,   C_ERR,   C_ERR,   C_ERR,   C_ERR,   C_ERR,   C_ERR,
  C_ERR,   C_SPACE, C_SPACE, C_ERR,   C_ERR,   C_SPACE, C_ERR,   C_ERR,
  C_ERR,   C_ERR,   C_ERR,   C_ERR,   C_ERR,   C_ERR,   C_ERR,   C_ERR,
  C_ERR,   C_ERR,   C_ERR,   C_ERR,   C_ERR,   C_ERR,   C_ERR,   C_ERR,

  C_SPACE, C_ETC,   C_QUOTE, C_ETC,   C_ETC,   C_ETC,   C_ETC,   C_ETC,
  C_ETC,   C_ETC,   C_ETC,   C_ETC,   C_COMMA, C_DIGIT, C_ETC,   C_ETC,
  C_DIGIT, C_DIGIT, C_DIGIT, C_DIGIT, C_DIGIT, C_DIGIT, C_DIGIT, C_DIGIT,
  C_DIGIT, C_DIGIT, C_COLON, C_ETC,   C_ETC,   C_ETC,   C_ETC,   C_ETC,

  C_ETC,   C_ETC,   C_ETC,   C_ETC,   C_ETC,   C_ETC,   C_ETC,   C_ETC,
  C_ETC,   C_ETC,   C_ETC,   C_ETC,   C_ETC,   C_ETC,   C_ETC,   C_ETC,
  C_ETC,   C_ETC,   C_ETC,   C_ETC,   C_ETC,   C_ETC,   C_ETC,   C_ETC,
  C_ETC,   C_ETC,   C_ETC,   C_LSQRB, C_ETC,   C_RSQRB, C_ETC,   C_ETC,

  C_ETC,   C_ETC,   C_ETC,   C_ETC,   C_ETC,   C_ETC,   C_LOW_F, C_ETC,
  C_ETC,   C_ETC,   C_ETC,   C_ETC,   C_ETC,   C_ETC,   C_LOW_N, C_ETC,
  C_ETC,   C_ETC,   C_ETC,   C_ETC,   C_LOW_T, C_ETC,   C_ETC,   C_ETC,
  C_ETC,   C_ETC,   C_ETC,   C_LCURB, C_ETC,   C_RCURB, C_ETC,   C_ETC
};


typedef int (*json_state_handler)(json_engine *);


/* The string is broken. */
static int er_en(json_engine *j)
{
  j->error= JE_EOS;
  return 1;
}


/* This symbol here breaks the JSON syntax. */
static int __(json_engine *j)
{
  j->error= JE_UEX;
  return 1;
}


/* Value of object. */
static int ky_ob(json_engine *j)
{
  j->state= json_engine::OB_B;
  *(++j->stack_p)= json_engine::OB_C;
  return 0;
}

/* Read value of object. */
static int rd_ob(json_engine *j)
{
  j->state= json_engine::OB_B;
  j->value_type= json_engine::OBJECT;
  j->value= j->value_begin;
  *(++j->stack_p)= json_engine::OB_C;
  return 0;
}


/* Value of array. */
static int ky_ar(json_engine *j)
{
  j->state= json_engine::AR_B;
  *(++j->stack_p)= json_engine::AR_C;
  j->value= j->value_begin;
  return 0;
}

/* Read value of object. */
static int rd_ar(json_engine *j)
{
  j->state= json_engine::AR_B;
  j->value_type= json_engine::ARRAY;
  j->value= j->value_begin;
  *(++j->stack_p)= json_engine::AR_C;
  return 0;
}



/* \b 8  \f 12 \n 10  \r 13 \t 9 */
enum json_s_classes {
  S_0= 0,
  S_1= 1,
  S_2= 2,
  S_3= 3,
  S_4= 4,
  S_5= 5,
  S_6= 6,
  S_7= 7,
  S_8= 8,
  S_9= 9,
  S_A= 10,
  S_B= 11,
  S_C= 12,
  S_D= 13,
  S_E= 14,
  S_F= 15,
  S_ETC= 36,    /* rest of characters. */
  S_QUOTE= 37,
  S_SOLID= 38, /* \ */
  S_ERR= 100,   /* disallowed */
};


/* This maps characters to their types inside a string constant. */
static const int json_instr_chr_map[128] = {
  S_ERR,   S_ERR,   S_ERR,   S_ERR,   S_ERR,   S_ERR,   S_ERR,   S_ERR,
  S_ERR,   S_ERR,   S_ERR,   S_ERR,   S_ERR,   S_ERR,   S_ERR,   S_ERR,
  S_ERR,   S_ERR,   S_ERR,   S_ERR,   S_ERR,   S_ERR,   S_ERR,   S_ERR,
  S_ERR,   S_ERR,   S_ERR,   S_ERR,   S_ERR,   S_ERR,   S_ERR,   S_ERR,

  S_ETC,   S_ETC,   S_QUOTE, S_ETC,   S_ETC,   S_ETC,   S_ETC,   S_ETC,
  S_ETC,   S_ETC,   S_ETC,   S_ETC,   S_ETC,   S_ETC,   S_ETC,   S_ETC,
  S_0,     S_1,     S_2,     S_3,     S_4,     S_5,     S_6,     S_7,
  S_8,     S_9,     S_ETC,   S_ETC,   S_ETC,   S_ETC,   S_ETC,   S_ETC,

  S_ETC,   S_A,     S_B,     S_C,     S_D,     S_E,     S_F,     S_ETC,
  S_ETC,   S_ETC,   S_ETC,   S_ETC,   S_ETC,   S_ETC,   S_ETC,   S_ETC,
  S_ETC,   S_ETC,   S_ETC,   S_ETC,   S_ETC,   S_ETC,   S_ETC,   S_ETC,
  S_ETC,   S_ETC,   S_ETC,   S_ETC,   S_SOLID, S_ETC,   S_ETC,   S_ETC,

  S_ETC,   S_A,     S_B,     S_C,     S_D,     S_E,     S_F,     S_ETC,
  S_ETC,   S_ETC,   S_ETC,   S_ETC,   S_ETC,   S_ETC,   S_ETC,   S_ETC,
  S_ETC,   S_ETC,   S_ETC,   S_ETC,   S_ETC,   S_ETC,   S_ETC,   S_ETC,
  S_ETC,   S_ETC,   S_ETC,   S_ETC,   S_ETC,   S_ETC,   S_ETC,   S_ETC
};


static int read_4digit(json_string *j, uchar *s)
{
  int i, t, c_len;
  for (i=0; i<4; i++)
  {
    if ((c_len= j->get_next_char()) <= 0)
      return j->error= j->eos() ? JE_EOS : JE_BAD;

    if (j->c_next >= 128 || (t= json_instr_chr_map[j->c_next]) >= S_F)
      return j->error= JE_UEX;

    j->c_str+= c_len;
    s[i/2]+= (i % 2) ? t : t*16;
  }
  return 0;
}


/* \b 8  \f 12 \n 10  \r 13 \t 9 */
int json_string::handle_esc()
{
  int t, c_len;
  
  if ((c_len= get_next_char()) <= 0)
    return error= eos() ? JE_EOS : JE_BAD;

  c_str+= c_len;
  switch (c_next)
  {
    case 'b':
      c_next= 8;
      return 0;
    case 'f':
      c_next= 12;
      return 0;
    case 'n':
      c_next= 10;
      return 0;
    case 'r':
      c_next= 13;
      return 0;
    case 't':
      c_next= 9;
      return 0;
  }

  if (c_next < 128 && (t= json_instr_chr_map[c_next]) == S_ERR)
  {
    c_str-= c_len;
    return error= JE_ESCAPING;
  }


  if (c_next != 'u')
    return 0;

  /*
    Read the four-hex-digits code.
    If symbol is not in the Basic Multilingual Plane, we're reading
    the string for the next four digits to compose the UTF-16 surrogate pair.
  */
  uchar s[4]= {0,0,0,0};

  if (read_4digit(this, s))
    return 1;
  
  if ((c_len= my_utf16_uni(0, &c_next, s, s+2)) == 2)
    return 0;

  if (c_len != MY_CS_TOOSMALL4)
    return error= JE_BAD;

  if ((c_len= get_next_char()) <= 0)
    return error= eos() ? JE_EOS : JE_BAD;
  if (c_next != '\\')
    return error= JE_UEX;

  if ((c_len= get_next_char()) <= 0)
    return error= eos() ? JE_EOS : JE_BAD;
  if (c_next != 'u')
    return error= JE_UEX;

  if (read_4digit(this, s+2))
    return 1;

  if ((c_len= my_utf16_uni(0, &c_next, s, s+4)) == 2)
    return 0;

  return error= JE_BAD;
}


int json_string::read_str_chr()
{
  int c_len;

  if ((c_len= get_next_char()) > 0)
  {
    c_str+= c_len;
    return (c_next == '\\') ? handle_esc() : 0;
  }
  error= eos() ? JE_EOS : JE_BAD; 
  return 1;
}


inline int pass_str(json_engine *j)
{
  int t, c_len;
  for (;;)
  {
    if ((c_len= j->get_next_char()) > 0)
    {
      j->c_str+= c_len;
      if (j->c_next >= 128 || ((t=json_instr_chr_map[j->c_next]) <= S_ETC))
        continue;

      if (j->c_next == '"')
        break;
      if (j->c_next == '\\')
      {
        if (j->handle_esc())
          return 1;
        continue;
      }
      /* Symbol not allowed in JSON. */
      return j->error= JE_IMP;
    }
    else
      return j->error= j->eos() ? JE_EOS : JE_BAD; 
  }

  j->state= *j->stack_p;
  return 0;
}


/* Scalar string. */
static int scr_s(json_engine *j)
{
  return pass_str(j) || j->scan_next();
}


/* Read scalar string. */
static int rd_s(json_engine *j)
{
  j->value= j->c_str;

  if (pass_str(j))
    return 1;

  j->state= *j->stack_p;
  j->value_type= json_engine::STRING;
  j->value_len= (j->c_str - j->value) - 1;
  return 0;
}


enum json_num_classes {
  N_MINUS,
  N_PLUS,
  N_ZERO,
  N_DIGIT,
  N_POINT,
  N_E,
  N_END,
  N_EEND,
  N_ERR,
  N_NUM_CLASSES
};


static int json_num_chr_map[128] = {
  N_ERR,   N_ERR,   N_ERR,   N_ERR,   N_ERR,   N_ERR,   N_ERR,   N_ERR,
  N_ERR,   N_END,   N_END,   N_ERR,   N_ERR,   N_END,   N_ERR,   N_ERR,
  N_ERR,   N_ERR,   N_ERR,   N_ERR,   N_ERR,   N_ERR,   N_ERR,   N_ERR,
  N_ERR,   N_ERR,   N_ERR,   N_ERR,   N_ERR,   N_ERR,   N_ERR,   N_ERR,

  N_END,   N_EEND,  N_EEND,  N_EEND,  N_EEND,  N_EEND,  N_EEND,  N_EEND,
  N_EEND,  N_EEND,  N_EEND,  N_PLUS,  N_END,   N_MINUS, N_POINT, N_EEND,
  N_ZERO,  N_DIGIT, N_DIGIT, N_DIGIT, N_DIGIT, N_DIGIT, N_DIGIT, N_DIGIT,
  N_DIGIT, N_DIGIT, N_EEND,  N_EEND,  N_EEND,  N_EEND,  N_EEND,  N_EEND,

  N_EEND,  N_EEND,  N_EEND,  N_EEND,  N_EEND,  N_E,     N_EEND,  N_EEND,
  N_EEND,  N_EEND,  N_EEND,  N_EEND,  N_EEND,  N_EEND,  N_EEND,  N_EEND,
  N_EEND,  N_EEND,  N_EEND,  N_EEND,  N_EEND,  N_EEND,  N_EEND,  N_EEND,
  N_EEND,  N_EEND,  N_EEND,  N_EEND,  N_EEND,  N_END,   N_EEND,  N_EEND,

  N_EEND,  N_EEND,  N_EEND,  N_EEND,  N_EEND,  N_E,     N_EEND,  N_EEND,
  N_EEND,  N_EEND,  N_EEND,  N_EEND,  N_EEND,  N_EEND,  N_EEND,  N_EEND,
  N_EEND,  N_EEND,  N_EEND,  N_EEND,  N_EEND,  N_EEND,  N_EEND,  N_EEND,
  N_EEND,  N_EEND,  N_EEND,  N_EEND,  C_ETC,   N_END,   N_EEND,  N_EEND,
};

enum json_num_states {
  NS_OK,
  NS_GO,
  NS_GO1,
  NS_Z,
  NS_Z1,
  NS_INT,
  NS_FRAC,
  NS_EX,
  NS_EX1,
  NS_NUM_STATES
};


static int json_num_states[NS_NUM_STATES][N_NUM_CLASSES]=
{
/*            -        +       0        1..9    POINT    E       END_OK   ERROR */
/* OK */   { JE_UEX,  JE_UEX, JE_UEX,   JE_UEX, JE_UEX,  JE_UEX, JE_UEX, JE_BAD },
/* GO */   { NS_GO1,  JE_UEX, NS_Z,     NS_INT, JE_UEX,  JE_UEX, JE_UEX, JE_BAD },
/* GO1 */  { JE_UEX,  JE_UEX, NS_Z1,    NS_INT, JE_UEX,  JE_UEX, JE_UEX, JE_BAD },
/* ZERO */ { JE_UEX,  JE_UEX, JE_UEX,   JE_UEX, NS_FRAC, JE_UEX, NS_OK,  JE_BAD },
/* ZE1 */  { JE_UEX,  JE_UEX, JE_UEX,   JE_UEX, NS_FRAC, JE_UEX, JE_UEX, JE_BAD },
/* INT */  { JE_UEX,  JE_UEX, NS_INT,   NS_INT, NS_FRAC, NS_EX,  NS_OK,  JE_BAD },
/* FRAC */ { JE_UEX,  JE_UEX, NS_FRAC,  NS_FRAC,JE_UEX,  NS_EX,  NS_OK,  JE_BAD },
/* EX */   { NS_EX1,  NS_EX1, NS_EX1,   NS_EX1, JE_UEX,  JE_UEX, JE_UEX, JE_BAD }, 
/* EX1 */  { JE_UEX,  JE_UEX, NS_EX1,   NS_EX1, JE_UEX,  JE_UEX, JE_UEX, JE_BAD }
};


inline int pass_n(json_engine *j)
{
  int state= json_num_states[NS_GO][json_num_chr_map[j->c_next]];
  int c_len;

  for (;;)
  {
    if ((c_len= j->get_next_char()) > 0)
    {
      if ((state= json_num_states[state][json_num_chr_map[j->c_next]]) > 0)
      {
        j->c_str+= c_len;
        continue;
      }
      break;
    }

    if ((j->error= j->eos() ? json_num_states[state][N_END] : JE_BAD) < 0)
      return 1;
    else
      break;
  }

  j->state= *j->stack_p;
  return 0;
}


/* Scalar numeric. */
static int scr_n(json_engine *j)
{
  return pass_n(j) || j->scan_next();
}


/* Read number. */
static int rd_n(json_engine *j)
{
  j->value= j->value_begin;
  if (pass_n(j) == 0)
  {
    j->value_type= json_engine::NUMBER;
    j->value_len= j->c_str - j->value_begin;
    return 0;
  }
  return 1;
}


static int assure_string(json_string *j, const char *str)
{
  int c_len;
  while (*str)
  {
    if ((c_len= j->get_next_char()) > 0)
    {
      if (j->c_next == (my_wc_t) *(str++))
      {
        j->c_str+= c_len;
        continue;
      }
      return JE_UEX;
    }
    return j->eos() ? JE_EOS : JE_BAD; 
  }

  return 0;
}


/* Scalar false. */
static int scr_f(json_engine *j)
{
  if (assure_string(j, "alse"))
   return 1;
  j->state= *j->stack_p;
  return j->scan_next();
}


/* Scalar null. */
static int scr_l(json_engine *j)
{
  if (assure_string(j, "ull"))
   return 1;
  j->state= *j->stack_p;
  return j->scan_next();
}


/* Scalar true. */
static int scr_t(json_engine *j)
{
  if (assure_string(j, "rue"))
   return 1;
  j->state= *j->stack_p;
  return j->scan_next();
}


/* Read false. */
static int rd_f(json_engine *j)
{
  j->value_type= json_engine::V_FALSE;
  j->value= j->value_begin;
  j->state= *j->stack_p;
  j->value_len= 5;
  return assure_string(j, "alse");
}


/* Read null. */
static int rd_l(json_engine *j)
{
  j->value_type= json_engine::V_NULL;
  j->value= j->value_begin;
  j->state= *j->stack_p;
  j->value_len= 4;
  return assure_string(j, "ull");
}


/* Read true. */
static int rd_t(json_engine *j)
{
  j->value_type= json_engine::V_TRUE;
  j->value= j->value_begin;
  j->state= *j->stack_p;
  j->value_len= 4;
  return assure_string(j, "rue");
}


/* Disallowed character. */
static int e_chr(json_engine *j)
{
  j->error= JE_IMP;
  return 1;
}


/* Bad character. */
static int b_chr(json_engine *j)
{
  j->error= JE_BAD;
  return 1;
}


/* Correct finish. */
static int done(json_engine *j)
{
  return 1;
}


/* End of the object. */
static int en_ob(json_engine *j)
{
  j->stack_p--;
  j->state= json_engine::OB_E;
  return 0;
}


/* End of the array. */
static int en_ar(json_engine *j)
{
  j->stack_p--;
  j->state= json_engine::AR_E;
  return 0;
}


/* Start reading key name. */
static int get_k(json_engine *j)
{
  j->state= json_engine::KEY;
  return 0;
}


inline void json_string::skip_s_getchar(int &t_next, int &c_len)
{
  do
  {
    if ((c_len= get_next_char()) <= 0)
      t_next= eos() ? C_EOS : C_BAD;
    else
    {
      t_next= (c_next < 128) ? json_chr_map[c_next] : C_ETC;
      c_str+= c_len;
    }
  } while (t_next == C_SPACE);
}


/* Next key name. */
static int nex_k(json_engine *j)
{
  int t_next, c_len;
  j->skip_s_getchar(t_next, c_len);

  if (t_next == C_QUOTE)
  {
    j->state= json_engine::KEY;
    return 0;
  }

  j->error= (t_next == C_EOS)  ? JE_EOS :
            ((t_next == C_BAD) ? JE_BAD :
                                 JE_UEX);
  return 1;
}


/* Forward declarations. */
static int skp_c(json_engine *j);
static int skp_k(json_engine *j);
static int ee_cb(json_engine *j);
static int ee_qb(json_engine *j);
static int ee_cm(json_engine *j);
static int ee_dn(json_engine *j);


static int ar_c(json_engine *j)
{
  j->state= json_engine::VALUE;
  return 0;
}


static int arb(json_engine *j)
{
  j->state= json_engine::VALUE;
  j->c_str-= j->sav_c_len;
  return 0;
}


static json_state_handler json_actions[json_engine::NR_STATES][NR_C_CLASSES]=
/*        EOS    {      }      [      ]      :      ,      "      -0..9  f      n      t      ETC ERR   BAD*/
{
/*VALUE*/{er_en, ky_ob, __,    ky_ar, __,    __,    __,    scr_s, scr_n, scr_f, scr_l, scr_t, __,    e_chr, b_chr},
/*DONE*/ {done,  __,    __,    __,    __,    __,    __,    __,    __,    __,    __,    __,    __,    e_chr, b_chr},
/*KEY*/  {er_en, skp_k, skp_k, skp_k, skp_k, skp_k, skp_k, skp_c, skp_k, skp_k, skp_k, skp_k, skp_k, e_chr, b_chr},
/*OB_B*/ {er_en, __,    en_ob, __,    __,    __,    __,    get_k, __,    __,    __,    __,    __,    e_chr, b_chr},
/*OB_E*/ {ee_dn, __,    ee_cb,  __,   ee_qb, __,    ee_cm, __,    __,    __,    __,    __,    __,    e_chr, b_chr},
/*OB_C*/ {er_en, __,    en_ob, __,    en_ar, __,    nex_k, __,    __,    __,    __,    __,    __,    e_chr, b_chr},
/*AR_B*/ {er_en, arb,   __,    arb,   en_ar, __,    __,    arb,   arb,   arb,   arb,   arb,   __,    e_chr, b_chr},
/*AR_E*/ {ee_dn, __,    ee_cb, __,    ee_qb, __,    ee_cm, __,    __,    __,    __,    __,    __,    e_chr, b_chr},
/*AR_C*/ {er_en, __,    __,    __,    en_ar, __,    ar_c,  __,    __,    __,    __,    __,    __,    e_chr, b_chr},
/*RD_V*/ {er_en, rd_ob, __,    rd_ar, __,    __,    __,    rd_s,  rd_n,  rd_f,  rd_l,  rd_t,  __,    e_chr, b_chr},
};


void json_string::set_cs(CHARSET_INFO *i_cs)
{
  cs= i_cs;
  error= 0;
  wc= i_cs->cset->mb_wc;
}


int json_engine::scan_start(CHARSET_INFO *i_cs, const uchar *str, const uchar *end)
{
  json_string::setup(i_cs, str, end);
  stack[0]= json_engine::DONE;
  stack_p= stack;
  state= json_engine::VALUE;
  return 0;
}


/* Skip colon and the value. */
static int skp_c(json_engine *j)
{
  int t_next, c_len;

  j->skip_s_getchar(t_next, c_len);

  if (t_next == C_COLON)
  {
    j->skip_s_getchar(t_next, c_len);
    return json_actions[json_engine::VALUE][t_next](j);
 }

  j->error= (t_next == C_EOS)  ? JE_EOS :
            ((t_next == C_BAD) ? JE_BAD :
                                 JE_UEX);

  return 1;
}


/* Skip colon and the value. */
static int skp_k(json_engine *j)
{
  int t_next, c_len;
  while (j->read_keyname_chr() == 0) {}

  if (j->error)
    return 1;

  j->skip_s_getchar(t_next, c_len);
  return json_actions[json_engine::VALUE][t_next](j);
}


/* Continue after the end of the structure. */
static int ee_dn(json_engine *j)
{ return json_actions[*j->stack_p][C_EOS](j); }


/* Continue after the end of the structure. */
static int ee_cb(json_engine *j)
{ return json_actions[*j->stack_p][C_RCURB](j); }


/* Continue after the end of the structure. */
static int ee_qb(json_engine *j)
{ return json_actions[*j->stack_p][C_RSQRB](j); }


/* Continue after the end of the structure. */
static int ee_cm(json_engine *j)
{ return json_actions[*j->stack_p][C_COMMA](j); }


int json_engine::read_keyname_chr()
{
  int c_len, t;

  if ((c_len= get_next_char()) > 0)
  {
    c_str+= c_len;
    if (c_next>= 128 || (t= json_instr_chr_map[c_next]) <= S_ETC)
      return 0;

    switch (t)
    {
    case S_QUOTE:
      for (;;)  /* Skip spaces until ':'. */
      {
        if ((c_len= get_next_char() > 0))
        {
          if (c_next == ':')
          {
            c_str+= c_len;
            state= VALUE;
            return 1;
          }

          if (c_next < 128 && json_chr_map[c_next] == C_SPACE)
          {
            c_str+= c_len;
            continue;
          }
          error= JE_UEX;
          break;
        }
        error= eos() ? JE_EOS : JE_BAD;
        break;
      }
      return 1;
    case S_SOLID:
      return handle_esc();
    case S_ERR:
      c_str-= c_len;
      error= JE_IMS;
      return 1;
    }
  }
  error= eos() ? JE_EOS : JE_BAD; 
  return 1;
}


int json_engine::read_value()
{
  int t_next, c_len, res;

  if (state == KEY)
  {
    while (read_keyname_chr() == 0) {}

    if (error)
      return 1;
  }

  skip_s_getchar(t_next, c_len);

  value_begin= c_str-c_len;
  res= json_actions[READ_VALUE][t_next](this);
  value_end= c_str;
  return res;
}


int json_engine::scan_next()
{
  int t_next;

  skip_s_getchar(t_next, sav_c_len);
  return json_actions[state][t_next](this);
}


enum json_path_c_classes {
  P_EOS,    /* end of string */
  P_USD,    /* $ */
  P_ASTER,  /* * */
  P_LSQRB,  /* [ */
  P_RSQRB,  /* ] */
  P_POINT,  /* . */
  P_ZERO,   /* 0 */
  P_DIGIT,  /* 123456789 */
  P_L,      /* l */
  P_S,      /* s */
  P_SPACE,  /* space */
  P_SOLID,  /* \ */
  P_ETC,    /* everything else */
  P_ERR,    /* character disallowed in JSON*/
  P_BAD,    /* invalid character */
  N_PATH_CLASSES,
};


static int json_path_chr_map[128] = {
  P_ERR,   P_ERR,   P_ERR,   P_ERR,   P_ERR,   P_ERR,   P_ERR,   P_ERR,
  P_ERR,   P_SPACE, P_SPACE, P_ERR,   P_ERR,   P_SPACE, P_ERR,   P_ERR,
  P_ERR,   P_ERR,   P_ERR,   P_ERR,   P_ERR,   P_ERR,   P_ERR,   P_ERR,
  P_ERR,   P_ERR,   P_ERR,   P_ERR,   P_ERR,   P_ERR,   P_ERR,   P_ERR,

  P_SPACE, P_ETC,   P_ETC,   P_ETC,   P_USD,   P_ETC,   P_ETC,   P_ETC,
  P_ETC,   P_ETC,   P_ASTER, P_ETC,   P_ETC,   P_ETC,   P_POINT, P_ETC,
  P_ZERO,  P_DIGIT, P_DIGIT, P_DIGIT, P_DIGIT, P_DIGIT, P_DIGIT, P_DIGIT,
  P_DIGIT, P_DIGIT, P_ETC,   P_ETC,   P_ETC,   P_ETC,   P_ETC,   P_ETC,

  P_ETC,   P_ETC,   P_ETC,   P_ETC,   P_ETC,   P_ETC,   P_ETC,   P_ETC,
  P_ETC,   P_ETC,   P_ETC,   P_ETC,   P_L,     P_ETC,   P_ETC,   P_ETC,
  P_ETC,   P_ETC,   P_S,     P_ETC,   P_ETC,   P_ETC,   P_ETC,   P_ETC,
  P_ETC,   P_ETC,   P_ETC,   P_LSQRB, P_SOLID, P_RSQRB, P_ETC,   P_ETC,

  P_ETC,   P_ETC,   P_ETC,   P_ETC,   P_ETC,   P_ETC,   P_ETC,   P_ETC,
  P_ETC,   P_ETC,   P_ETC,   P_ETC,   P_L,     P_ETC,   P_ETC,   P_ETC,
  P_ETC,   P_ETC,   P_S,     P_ETC,   P_ETC,   P_ETC,   P_ETC,   P_ETC,
  P_ETC,   P_ETC,   P_ETC,   P_ETC,   P_ETC,   P_ETC,   P_ETC,   P_ETC
};


enum json_path_states {
  PS_GO,
  PS_LAX,
  PS_PT,
  PS_AR,
  PS_AWD,
  PS_Z,
  PS_INT,
  PS_AS,
  PS_KEY,
  PS_KNM,
  PS_KWD,
  N_PATH_STATES,
  PS_SCT,
  PS_EKY,
  PS_EAR,
  PS_ESC,
  PS_OK,
  PS_KOK
};


static int json_path_transitions[N_PATH_STATES][N_PATH_CLASSES]=
{
/*        EOS       $,      *       [       ]       .       0       1..9    L       S       SPACE   SOLID   ETC     ERR     BAD  */
/* GO  */ { JE_EOS, PS_PT,  JE_UEX, JE_UEX, JE_UEX, JE_UEX, JE_UEX, JE_UEX, PS_LAX, PS_SCT, PS_GO,  JE_UEX, JE_UEX, JE_IMP, JE_BAD},
/* LAX */ { JE_EOS, JE_UEX, JE_UEX, JE_UEX, JE_UEX, JE_UEX, JE_UEX, JE_UEX, PS_LAX, JE_UEX, PS_GO,  JE_UEX, JE_UEX, JE_IMP, JE_BAD},
/* PT */  { PS_OK,  JE_UEX, JE_UEX, PS_AR,  JE_UEX, PS_KEY, JE_UEX, JE_UEX, JE_UEX, JE_UEX, JE_UEX, JE_UEX, JE_UEX, JE_IMP, JE_BAD},
/* AR */  { JE_EOS, JE_UEX, PS_AWD, JE_UEX, PS_PT,  JE_UEX, PS_Z,   PS_INT, JE_UEX, JE_UEX, PS_AR,  JE_UEX, JE_UEX, JE_IMP, JE_BAD},
/* AWD */ { JE_EOS, JE_UEX, JE_UEX, JE_UEX, PS_PT,  JE_UEX, JE_UEX, JE_UEX, JE_UEX, JE_UEX, PS_AS,  JE_UEX, JE_UEX, JE_IMP, JE_BAD},
/* Z */   { JE_EOS, JE_UEX, JE_UEX, JE_UEX, PS_PT,  JE_UEX, JE_UEX, JE_UEX, JE_UEX, JE_UEX, PS_AS,  JE_UEX, JE_UEX, JE_IMP, JE_BAD},
/* INT */ { JE_EOS, JE_UEX, JE_UEX, JE_UEX, PS_PT,  JE_UEX, PS_INT, PS_INT, JE_UEX, JE_UEX, PS_AS,  JE_UEX, JE_UEX, JE_IMP, JE_BAD},
/* AS */  { JE_EOS, JE_UEX, JE_UEX, JE_UEX, PS_PT,  JE_UEX, JE_UEX, JE_UEX, JE_UEX, JE_UEX, PS_AS,  JE_UEX, JE_UEX, JE_IMP, JE_BAD},
/* KEY */ { JE_EOS, PS_KNM, PS_KWD, JE_UEX, PS_KNM, JE_UEX, PS_KNM, PS_KNM, PS_KNM, PS_KNM, PS_KNM, JE_UEX, PS_KNM, JE_IMP, JE_BAD},
/* KNM */ { PS_KOK, PS_KNM, PS_KNM, PS_EAR, PS_KNM, PS_EKY, PS_KNM, PS_KNM, PS_KNM, PS_KNM, PS_KNM, PS_ESC, PS_KNM, JE_IMP, JE_BAD},
/* KWD */ { PS_OK,  JE_UEX, JE_UEX, PS_AR,  JE_UEX, PS_EKY, JE_UEX, JE_UEX, JE_UEX, JE_UEX, JE_UEX, JE_UEX, JE_UEX, JE_IMP, JE_BAD}
};


int json_path::setup(CHARSET_INFO *i_cs, const uchar *str, const uchar *end)
{
  int c_len, t_next, state= PS_GO;

  json_string::setup(i_cs, str, end);

  steps[0].type= json_path_step::ARRAY;
  steps[0].wild= 1;
  last_step= steps;
  mode_strict= false;

  do
  {
    if ((c_len= get_next_char()) <= 0)
      t_next= eos() ? P_EOS : P_BAD;
    else
      t_next= (c_next >= 128) ? P_ETC : json_path_chr_map[c_next];

    if ((state= json_path_transitions[state][t_next]) < 0)
      return error= state;

    c_str+= c_len;

    switch (state)
    {
    case PS_LAX:
      if ((error= assure_string(this, "ax")))
        return 1;
      mode_strict= false;
      continue;
    case PS_SCT:
      if ((error= assure_string(this, "rict")))
        return 1;
      mode_strict= true;
      state= PS_LAX;
      continue;
    case PS_AWD:
      last_step->wild= 1;
      continue;
    case PS_INT:
      last_step->n_item*= 10;
      last_step->n_item+= c_next - '0';
      continue;
    case PS_EKY:
      last_step->key_end= c_str - c_len;
      state= PS_KEY;
      /* Note no 'continue' here. */
    case PS_KEY:
      last_step++;
      last_step->type= json_path_step::KEY;
      last_step->wild= 0;
      last_step->key= c_str;
      continue;
    case PS_EAR:
      last_step->key_end= c_str - c_len;
      state= PS_AR;
      /* Note no 'continue' here. */
    case PS_AR:
      last_step++;
      last_step->type= json_path_step::ARRAY;
      last_step->wild= 0;
      last_step->n_item= 0;
      continue;
    case PS_KWD:
      last_step->wild= 1;
      continue;
    case PS_ESC:
      if (handle_esc())
        return 1;
      continue;
    case PS_KOK:
      last_step->key_end= c_str - c_len;
      state= PS_OK;
      break;
    };
  } while (state != PS_OK);

  return 0;
}


int json_engine::skip_level()
{
  int ct= 0;

  while (scan_next() == 0)
  {
    switch (state) {
    case OB_B:
    case AR_B:
      ct++;
      break;
    case OB_E:
    case AR_E:
      if (ct == 0)
        return 0;
      ct--;
      break;
    }
  }

  return 1;
}


int json_engine::skip_key()
{
  if (read_value())
    return 1;

  if (value_scalar())
    return 0;

  return skip_level();
}
