/* Symbol table manager for Bison.

   Copyright (C) 1984, 1989, 2000-2002, 2004-2013 Free Software
   Foundation, Inc.

   This file is part of Bison, the GNU Compiler Compiler.

   This program is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, either version 3 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program.  If not, see <http://www.gnu.org/licenses/>.  */

#include <config.h>
#include "system.h"

#include <hash.h>

#include "complain.h"
#include "gram.h"
#include "symtab.h"

/*-------------------------------------------------------------------.
| Symbols sorted by tag.  Allocated by the first invocation of       |
| symbols_do, after which no more symbols should be created.         |
`-------------------------------------------------------------------*/

static symbol **symbols_sorted = NULL;
static symbol **semantic_types_sorted = NULL;

/*------------------------.
| Distinguished symbols.  |
`------------------------*/

symbol *errtoken = NULL;
symbol *undeftoken = NULL;
symbol *endtoken = NULL;
symbol *accept = NULL;
symbol *startsymbol = NULL;
location startsymbol_location;

/*---------------------------.
| Precedence relation graph. |
`---------------------------*/

static symgraph **prec_nodes;
/* Number of groups created in the precedence graph. */
static int ngroups;
/* In the first bit, store whether the node has been visited as a father in
 * the current pass of node grouping. The second bit store whether is has been
 * visited as a son. */
static intvect *markvector;

/*-----------------------------------.
| Store which associativity is used. |
`-----------------------------------*/

bool *used_assoc = NULL;

/*---------------------------------.
| Create a new symbol, named TAG.  |
`---------------------------------*/

static symbol *
symbol_new (uniqstr tag, location loc)
{
  symbol *res = xmalloc (sizeof *res);
  uniqstr_assert (tag);

  /* If the tag is not a string (starts with a double quote), check
     that it is valid for Yacc. */
  if (tag[0] != '\"' && tag[0] != '\'' && strchr (tag, '-'))
    complain (&loc, Wyacc,
              _("POSIX Yacc forbids dashes in symbol names: %s"), tag);

  res->tag = tag;
  res->location = loc;

  res->type_name = NULL;
  {
    int i;
    for (i = 0; i < CODE_PROPS_SIZE; ++i)
      code_props_none_init (&res->props[i]);
  }

  res->number = NUMBER_UNDEFINED;
  res->prec = 0;
  res->assoc = undef_assoc;
  res->user_token_number = USER_NUMBER_UNDEFINED;

  res->alias = NULL;
  res->class = unknown_sym;
  res->status = undeclared;

  if (nsyms == SYMBOL_NUMBER_MAXIMUM)
    complain (NULL, fatal, _("too many symbols in input grammar (limit is %d)"),
              SYMBOL_NUMBER_MAXIMUM);
  nsyms++;
  return res;
}

char const *
code_props_type_string (code_props_type kind)
{
  switch (kind)
    {
    case destructor:
      return "%destructor";
    case printer:
      return "%printer";
    }
  assert (0);
}

/*----------------------------------------.
| Create a new semantic type, named TAG.  |
`----------------------------------------*/

static semantic_type *
semantic_type_new (uniqstr tag, const location *loc)
{
  semantic_type *res = xmalloc (sizeof *res);

  uniqstr_assert (tag);
  res->tag = tag;
  res->location = loc ? *loc : empty_location;
  res->status = undeclared;
  {
    int i;
    for (i = 0; i < CODE_PROPS_SIZE; ++i)
      code_props_none_init (&res->props[i]);
  }

  return res;
}


/*-----------------.
| Print a symbol.  |
`-----------------*/

#define SYMBOL_ATTR_PRINT(Attr)                         \
  if (s->Attr)                                          \
    fprintf (f, " %s { %s }", #Attr, s->Attr)

#define SYMBOL_CODE_PRINT(Attr)                                         \
  if (s->props[Attr].code)                                              \
    fprintf (f, " %s { %s }", #Attr, s->props[Attr].code)

void
symbol_print (symbol const *s, FILE *f)
{
  if (s)
    {
      fprintf (f, "\"%s\"", s->tag);
      SYMBOL_ATTR_PRINT (type_name);
      SYMBOL_CODE_PRINT (destructor);
      SYMBOL_CODE_PRINT (printer);
    }
  else
    fprintf (f, "<NULL>");
}

#undef SYMBOL_ATTR_PRINT
#undef SYMBOL_CODE_PRINT


/*----------------------------------.
| Whether S is a valid identifier.  |
`----------------------------------*/

static bool
is_identifier (uniqstr s)
{
  static char const alphanum[26 + 26 + 1 + 10] =
    "abcdefghijklmnopqrstuvwxyz"
    "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
    "_"
    "0123456789";
  if (!s || ! memchr (alphanum, *s, sizeof alphanum - 10))
    return false;
  for (++s; *s; ++s)
    if (! memchr (alphanum, *s, sizeof alphanum))
      return false;
  return true;
}


/*-----------------------------------------------.
| Get the identifier associated to this symbol.  |
`-----------------------------------------------*/
uniqstr
symbol_id_get (symbol const *sym)
{
  aver (sym->user_token_number != USER_NUMBER_HAS_STRING_ALIAS);
  if (sym->alias)
    sym = sym->alias;
  return is_identifier (sym->tag) ? sym->tag : 0;
}


/*------------------------------------------------------------------.
| Complain that S's WHAT is redeclared at SECOND, and was first set |
| at FIRST.                                                         |
`------------------------------------------------------------------*/

static void
symbol_redeclaration (symbol *s, const char *what, location first,
                      location second)
{
  unsigned i = 0;
  complain_indent (&second, complaint, &i,
                   _("%s redeclaration for %s"), what, s->tag);
  i += SUB_INDENT;
  complain_indent (&first, complaint, &i,
                   _("previous declaration"));
}

static void
semantic_type_redeclaration (semantic_type *s, const char *what, location first,
                             location second)
{
  unsigned i = 0;
  complain_indent (&second, complaint, &i,
                   _("%s redeclaration for <%s>"), what, s->tag);
  i += SUB_INDENT;
  complain_indent (&first, complaint, &i,
                   _("previous declaration"));
}



/*-----------------------------------------------------------------.
| Set the TYPE_NAME associated with SYM.  Does nothing if passed 0 |
| as TYPE_NAME.                                                    |
`-----------------------------------------------------------------*/

void
symbol_type_set (symbol *sym, uniqstr type_name, location loc)
{
  if (type_name)
    {
      if (sym->type_name)
        symbol_redeclaration (sym, "%type", sym->type_location, loc);
      uniqstr_assert (type_name);
      sym->type_name = type_name;
      sym->type_location = loc;
    }
}

/*--------------------------------------------------------.
| Set the DESTRUCTOR or PRINTER associated with the SYM.  |
`--------------------------------------------------------*/

void
symbol_code_props_set (symbol *sym, code_props_type kind,
                       code_props const *code)
{
  if (sym->props[kind].code)
    symbol_redeclaration (sym, code_props_type_string (kind),
                          sym->props[kind].location,
                          code->location);
  sym->props[kind] = *code;
}

/*-----------------------------------------------------.
| Set the DESTRUCTOR or PRINTER associated with TYPE.  |
`-----------------------------------------------------*/

void
semantic_type_code_props_set (semantic_type *type,
                              code_props_type kind,
                              code_props const *code)
{
  if (type->props[kind].code)
    semantic_type_redeclaration (type, code_props_type_string (kind),
                                 type->props[kind].location,
                                 code->location);
  type->props[kind] = *code;
}

/*---------------------------------------------------.
| Get the computed %destructor or %printer for SYM.  |
`---------------------------------------------------*/

code_props *
symbol_code_props_get (symbol *sym, code_props_type kind)
{
  /* Per-symbol code props.  */
  if (sym->props[kind].code)
    return &sym->props[kind];

  /* Per-type code props.  */
  if (sym->type_name)
    {
      code_props *code =
        &semantic_type_get (sym->type_name, NULL)->props[kind];
      if (code->code)
        return code;
    }

  /* Apply default code props's only to user-defined symbols.  */
  if (sym->tag[0] != '$' && sym != errtoken)
    {
      code_props *code =
        &semantic_type_get (sym->type_name ? "*" : "", NULL)->props[kind];
      if (code->code)
        return code;
    }
  return &code_props_none;
}

/*-----------------------------------------------------------------.
| Set the PRECEDENCE associated with SYM.  Does nothing if invoked |
| with UNDEF_ASSOC as ASSOC.                                       |
`-----------------------------------------------------------------*/

void
symbol_precedence_set (symbol *sym, int prec, assoc a, location loc)
{
  if (a != undef_assoc)
    {
      if (sym->prec != 0)
        symbol_redeclaration (sym, assoc_to_string (a), sym->prec_location,
                              loc);
      sym->prec = prec;
      sym->assoc = a;
      sym->prec_location = loc;
    }

  /* Only terminals have a precedence. */
  symbol_class_set (sym, token_sym, loc, false);
}


/*------------------------------------.
| Set the CLASS associated with SYM.  |
`------------------------------------*/

void
symbol_class_set (symbol *sym, symbol_class class, location loc, bool declaring)
{
  bool warned = false;
  if (sym->class != unknown_sym && sym->class != class)
    {
      complain (&loc, complaint, _("symbol %s redefined"), sym->tag);
      /* Don't report both "redefined" and "redeclared".  */
      warned = true;
    }

  if (class == nterm_sym && sym->class != nterm_sym)
    sym->number = nvars++;
  else if (class == token_sym && sym->number == NUMBER_UNDEFINED)
    sym->number = ntokens++;

  sym->class = class;

  if (declaring)
    {
      if (sym->status == declared && !warned)
        complain (&loc, Wother, _("symbol %s redeclared"), sym->tag);
      sym->status = declared;
    }
}


/*------------------------------------------------.
| Set the USER_TOKEN_NUMBER associated with SYM.  |
`------------------------------------------------*/

void
symbol_user_token_number_set (symbol *sym, int user_token_number, location loc)
{
  int *user_token_numberp;

  if (sym->user_token_number != USER_NUMBER_HAS_STRING_ALIAS)
    user_token_numberp = &sym->user_token_number;
  else
    user_token_numberp = &sym->alias->user_token_number;
  if (*user_token_numberp != USER_NUMBER_UNDEFINED
      && *user_token_numberp != user_token_number)
    complain (&loc, complaint, _("redefining user token number of %s"),
              sym->tag);

  *user_token_numberp = user_token_number;
  /* User defined $end token? */
  if (user_token_number == 0)
    {
      endtoken = sym;
      /* It is always mapped to 0, so it was already counted in
         NTOKENS.  */
      if (endtoken->number != NUMBER_UNDEFINED)
        --ntokens;
      endtoken->number = 0;
    }
}


/*----------------------------------------------------------.
| If SYM is not defined, report an error, and consider it a |
| nonterminal.                                              |
`----------------------------------------------------------*/

static inline bool
symbol_check_defined (symbol *sym)
{
  if (sym->class == unknown_sym)
    {
      assert (sym->status != declared);
      complain (&sym->location,
                sym->status == needed ? complaint : Wother,
                _("symbol %s is used, but is not defined as a token"
                  " and has no rules"),
                  sym->tag);
      sym->class = nterm_sym;
      sym->number = nvars++;
    }

  {
    int i;
    for (i = 0; i < 2; ++i)
      symbol_code_props_get (sym, i)->is_used = true;
  }

  /* Set the semantic type status associated to the current symbol to
     'declared' so that we could check semantic types unnecessary uses. */
  if (sym->type_name)
    {
      semantic_type *sem_type = semantic_type_get (sym->type_name, NULL);
      if (sem_type)
        sem_type->status = declared;
    }

  return true;
}

static inline bool
semantic_type_check_defined (semantic_type *sem_type)
{
  /* <*> and <> do not have to be "declared".  */
  if (sem_type->status == declared
      || !*sem_type->tag
      || STREQ(sem_type->tag, "*"))
    {
      int i;
      for (i = 0; i < 2; ++i)
        if (sem_type->props[i].kind != CODE_PROPS_NONE
            && ! sem_type->props[i].is_used)
          complain (&sem_type->location, Wother,
                    _("useless %s for type <%s>"),
                    code_props_type_string (i), sem_type->tag);
    }
  else
    complain (&sem_type->location, Wother,
              _("type <%s> is used, but is not associated to any symbol"),
              sem_type->tag);

  return true;
}

static bool
symbol_check_defined_processor (void *sym, void *null ATTRIBUTE_UNUSED)
{
  return symbol_check_defined (sym);
}

static bool
semantic_type_check_defined_processor (void *sem_type,
                                       void *null ATTRIBUTE_UNUSED)
{
  return semantic_type_check_defined (sem_type);
}


void
symbol_make_alias (symbol *sym, symbol *str, location loc)
{
  if (str->alias)
    complain (&loc, Wother,
              _("symbol %s used more than once as a literal string"), str->tag);
  else if (sym->alias)
    complain (&loc, Wother,
              _("symbol %s given more than one literal string"), sym->tag);
  else
    {
      str->class = token_sym;
      str->user_token_number = sym->user_token_number;
      sym->user_token_number = USER_NUMBER_HAS_STRING_ALIAS;
      str->alias = sym;
      sym->alias = str;
      str->number = sym->number;
      symbol_type_set (str, sym->type_name, loc);
    }
}


/*---------------------------------------------------------.
| Check that THIS, and its alias, have same precedence and |
| associativity.                                           |
`---------------------------------------------------------*/

static inline void
symbol_check_alias_consistency (symbol *this)
{
  symbol *sym = this;
  symbol *str = this->alias;

  /* Check only the symbol in the symbol-string pair.  */
  if (!(this->alias
        && this->user_token_number == USER_NUMBER_HAS_STRING_ALIAS))
    return;

  if (str->type_name != sym->type_name)
    {
      if (str->type_name)
        symbol_type_set (sym, str->type_name, str->type_location);
      else
        symbol_type_set (str, sym->type_name, sym->type_location);
    }


  {
    int i;
    for (i = 0; i < CODE_PROPS_SIZE; ++i)
      if (str->props[i].code)
        symbol_code_props_set (sym, i, &str->props[i]);
      else if (sym->props[i].code)
        symbol_code_props_set (str, i, &sym->props[i]);
  }

  if (sym->prec || str->prec)
    {
      if (str->prec)
        symbol_precedence_set (sym, str->prec, str->assoc,
                               str->prec_location);
      else
        symbol_precedence_set (str, sym->prec, sym->assoc,
                               sym->prec_location);
    }
}

static bool
symbol_check_alias_consistency_processor (void *this,
                                          void *null ATTRIBUTE_UNUSED)
{
  symbol_check_alias_consistency (this);
  return true;
}


/*-------------------------------------------------------------------.
| Assign a symbol number, and write the definition of the token name |
| into FDEFINES.  Put in SYMBOLS.                                    |
`-------------------------------------------------------------------*/

static inline bool
symbol_pack (symbol *this)
{
  aver (this->number != NUMBER_UNDEFINED);
  if (this->class == nterm_sym)
    this->number += ntokens;
  else if (this->user_token_number == USER_NUMBER_HAS_STRING_ALIAS)
    return true;

  symbols[this->number] = this;
  return true;
}

static bool
symbol_pack_processor (void *this, void *null ATTRIBUTE_UNUSED)
{
  return symbol_pack (this);
}


static void
user_token_number_redeclaration (int num, symbol *first, symbol *second)
{
  unsigned i = 0;
  /* User token numbers are not assigned during the parsing, but in a
     second step, via a traversal of the symbol table sorted on tag.

     However, error messages make more sense if we keep the first
     declaration first.  */
  if (location_cmp (first->location, second->location) > 0)
    {
      symbol* tmp = first;
      first = second;
      second = tmp;
    }
  complain_indent (&second->location, complaint, &i,
                   _("user token number %d redeclaration for %s"),
                   num, second->tag);
  i += SUB_INDENT;
  complain_indent (&first->location, complaint, &i,
                   _("previous declaration for %s"),
                   first->tag);
}

/*--------------------------------------------------.
| Put THIS in TOKEN_TRANSLATIONS if it is a token.  |
`--------------------------------------------------*/

static inline bool
symbol_translation (symbol *this)
{
  /* Non-terminal? */
  if (this->class == token_sym
      && this->user_token_number != USER_NUMBER_HAS_STRING_ALIAS)
    {
      /* A token which translation has already been set? */
      if (token_translations[this->user_token_number] != undeftoken->number)
        user_token_number_redeclaration
          (this->user_token_number,
           symbols[token_translations[this->user_token_number]],
           this);

      token_translations[this->user_token_number] = this->number;
    }

  return true;
}

static bool
symbol_translation_processor (void *this, void *null ATTRIBUTE_UNUSED)
{
  return symbol_translation (this);
}


/*---------------------------------------.
| Symbol and semantic type hash tables.  |
`---------------------------------------*/

/* Initial capacity of symbol and semantic type hash table.  */
#define HT_INITIAL_CAPACITY 257

static struct hash_table *symbol_table = NULL;
static struct hash_table *semantic_type_table = NULL;

static inline bool
hash_compare_symbol (const symbol *m1, const symbol *m2)
{
  /* Since tags are unique, we can compare the pointers themselves.  */
  return UNIQSTR_EQ (m1->tag, m2->tag);
}

static inline bool
hash_compare_semantic_type (const semantic_type *m1, const semantic_type *m2)
{
  /* Since names are unique, we can compare the pointers themselves.  */
  return UNIQSTR_EQ (m1->tag, m2->tag);
}

static bool
hash_symbol_comparator (void const *m1, void const *m2)
{
  return hash_compare_symbol (m1, m2);
}

static bool
hash_semantic_type_comparator (void const *m1, void const *m2)
{
  return hash_compare_semantic_type (m1, m2);
}

static inline size_t
hash_symbol (const symbol *m, size_t tablesize)
{
  /* Since tags are unique, we can hash the pointer itself.  */
  return ((uintptr_t) m->tag) % tablesize;
}

static inline size_t
hash_semantic_type (const semantic_type *m, size_t tablesize)
{
  /* Since names are unique, we can hash the pointer itself.  */
  return ((uintptr_t) m->tag) % tablesize;
}

static size_t
hash_symbol_hasher (void const *m, size_t tablesize)
{
  return hash_symbol (m, tablesize);
}

static size_t
hash_semantic_type_hasher (void const *m, size_t tablesize)
{
  return hash_semantic_type (m, tablesize);
}

/*-------------------------------.
| Create the symbol hash table.  |
`-------------------------------*/

void
symbols_new (void)
{
  symbol_table = hash_initialize (HT_INITIAL_CAPACITY,
                                  NULL,
                                  hash_symbol_hasher,
                                  hash_symbol_comparator,
                                  free);
  semantic_type_table = hash_initialize (HT_INITIAL_CAPACITY,
                                         NULL,
                                         hash_semantic_type_hasher,
                                         hash_semantic_type_comparator,
                                         free);
}


/*----------------------------------------------------------------.
| Find the symbol named KEY, and return it.  If it does not exist |
| yet, create it.                                                 |
`----------------------------------------------------------------*/

symbol *
symbol_from_uniqstr (const uniqstr key, location loc)
{
  symbol probe;
  symbol *entry;

  probe.tag = key;
  entry = hash_lookup (symbol_table, &probe);

  if (!entry)
    {
      /* First insertion in the hash. */
      aver (!symbols_sorted);
      entry = symbol_new (key, loc);
      if (!hash_insert (symbol_table, entry))
        xalloc_die ();
    }
  return entry;
}


/*-----------------------------------------------------------------------.
| Find the semantic type named KEY, and return it.  If it does not exist |
| yet, create it.                                                        |
`-----------------------------------------------------------------------*/

semantic_type *
semantic_type_from_uniqstr (const uniqstr key, const location *loc)
{
  semantic_type probe;
  semantic_type *entry;

  probe.tag = key;
  entry = hash_lookup (semantic_type_table, &probe);

  if (!entry)
    {
      /* First insertion in the hash. */
      entry = semantic_type_new (key, loc);
      if (!hash_insert (semantic_type_table, entry))
        xalloc_die ();
    }
  return entry;
}


/*----------------------------------------------------------------.
| Find the symbol named KEY, and return it.  If it does not exist |
| yet, create it.                                                 |
`----------------------------------------------------------------*/

symbol *
symbol_get (const char *key, location loc)
{
  return symbol_from_uniqstr (uniqstr_new (key), loc);
}


/*-----------------------------------------------------------------------.
| Find the semantic type named KEY, and return it.  If it does not exist |
| yet, create it.                                                        |
`-----------------------------------------------------------------------*/

semantic_type *
semantic_type_get (const char *key, const location *loc)
{
  return semantic_type_from_uniqstr (uniqstr_new (key), loc);
}


/*------------------------------------------------------------------.
| Generate a dummy nonterminal, whose name cannot conflict with the |
| user's names.                                                     |
`------------------------------------------------------------------*/

symbol *
dummy_symbol_get (location loc)
{
  /* Incremented for each generated symbol.  */
  static int dummy_count = 0;
  static char buf[256];

  symbol *sym;

  sprintf (buf, "$@%d", ++dummy_count);
  sym = symbol_get (buf, loc);
  sym->class = nterm_sym;
  sym->number = nvars++;
  return sym;
}

bool
symbol_is_dummy (const symbol *sym)
{
  return sym->tag[0] == '@' || (sym->tag[0] == '$' && sym->tag[1] == '@');
}

/*-------------------.
| Free the symbols.  |
`-------------------*/

void
symbols_free (void)
{
  hash_free (symbol_table);
  hash_free (semantic_type_table);
  free (symbols);
  free (symbols_sorted);
  free (semantic_types_sorted);
}


/*---------------------------------------------------------------.
| Look for undefined symbols, report an error, and consider them |
| terminals.                                                     |
`---------------------------------------------------------------*/

static int
symbols_cmp (symbol const *a, symbol const *b)
{
  return strcmp (a->tag, b->tag);
}

static int
symbols_cmp_qsort (void const *a, void const *b)
{
  return symbols_cmp (*(symbol * const *)a, *(symbol * const *)b);
}

static void
symbols_do (Hash_processor processor, void *processor_data,
            struct hash_table *table, symbol ***sorted)
{
  size_t count = hash_get_n_entries (table);
  if (!*sorted)
    {
      *sorted = xnmalloc (count, sizeof **sorted);
      hash_get_entries (table, (void**)*sorted, count);
      qsort (*sorted, count, sizeof **sorted, symbols_cmp_qsort);
    }
  {
    size_t i;
    for (i = 0; i < count; ++i)
      processor ((*sorted)[i], processor_data);
  }
}

/*--------------------------------------------------------------.
| Check that all the symbols are defined.  Report any undefined |
| symbols and consider them nonterminals.                       |
`--------------------------------------------------------------*/

void
symbols_check_defined (void)
{
  symbols_do (symbol_check_defined_processor, NULL,
              symbol_table, &symbols_sorted);
  symbols_do (semantic_type_check_defined_processor, NULL,
              semantic_type_table, &semantic_types_sorted);
}

/*------------------------------------------------------------------.
| Set TOKEN_TRANSLATIONS.  Check that no two symbols share the same |
| number.                                                           |
`------------------------------------------------------------------*/

static void
symbols_token_translations_init (void)
{
  bool num_256_available_p = true;
  int i;

  /* Find the highest user token number, and whether 256, the POSIX
     preferred user token number for the error token, is used.  */
  max_user_token_number = 0;
  for (i = 0; i < ntokens; ++i)
    {
      symbol *this = symbols[i];
      if (this->user_token_number != USER_NUMBER_UNDEFINED)
        {
          if (this->user_token_number > max_user_token_number)
            max_user_token_number = this->user_token_number;
          if (this->user_token_number == 256)
            num_256_available_p = false;
        }
    }

  /* If 256 is not used, assign it to error, to follow POSIX.  */
  if (num_256_available_p
      && errtoken->user_token_number == USER_NUMBER_UNDEFINED)
    errtoken->user_token_number = 256;

  /* Set the missing user numbers. */
  if (max_user_token_number < 256)
    max_user_token_number = 256;

  for (i = 0; i < ntokens; ++i)
    {
      symbol *this = symbols[i];
      if (this->user_token_number == USER_NUMBER_UNDEFINED)
        this->user_token_number = ++max_user_token_number;
      if (this->user_token_number > max_user_token_number)
        max_user_token_number = this->user_token_number;
    }

  token_translations = xnmalloc (max_user_token_number + 1,
                                 sizeof *token_translations);

  /* Initialize all entries for literal tokens to the internal token
     number for $undefined, which represents all invalid inputs.  */
  for (i = 0; i < max_user_token_number + 1; i++)
    token_translations[i] = undeftoken->number;
  symbols_do (symbol_translation_processor, NULL,
              symbol_table, &symbols_sorted);
}


/*----------------------------------------------------------------.
| Assign symbol numbers, and write definition of token names into |
| FDEFINES.  Set up vectors SYMBOL_TABLE, TAGS of symbols.        |
`----------------------------------------------------------------*/

void
symbols_pack (void)
{
  symbols_do (symbol_check_alias_consistency_processor, NULL,
              symbol_table, &symbols_sorted);

  symbols = xcalloc (nsyms, sizeof *symbols);
  symbols_do (symbol_pack_processor, NULL, symbol_table, &symbols_sorted);

  /* Aliases leave empty slots in symbols, so remove them.  */
  {
    int writei;
    int readi;
    int nsyms_old = nsyms;
    for (writei = 0, readi = 0; readi < nsyms_old; readi += 1)
      {
        if (symbols[readi] == NULL)
          {
            nsyms -= 1;
            ntokens -= 1;
          }
        else
          {
            symbols[writei] = symbols[readi];
            symbols[writei]->number = writei;
            if (symbols[writei]->alias)
              symbols[writei]->alias->number = writei;
            writei += 1;
          }
      }
  }
  symbols = xnrealloc (symbols, nsyms, sizeof *symbols);

  symbols_token_translations_init ();

  if (startsymbol->class == unknown_sym)
    complain (&startsymbol_location, fatal,
              _("the start symbol %s is undefined"),
              startsymbol->tag);
  else if (startsymbol->class == token_sym)
    complain (&startsymbol_location, fatal,
              _("the start symbol %s is a token"),
              startsymbol->tag);
}

/*---------------------------------.
| Initialize relation graph nodes. |
`---------------------------------*/

static void
init_prec_nodes (void)
{
  int i;
  prec_nodes = xcalloc (nsyms, sizeof *prec_nodes);
  for (i = 0; i < nsyms; ++i)
    {
      prec_nodes[i] = xmalloc (sizeof *prec_nodes[i]);
      symgraph *s = prec_nodes[i];
      s->id = i;
      s->succ = 0;
      s->pred = 0;
      s->groupnext = 0;
      s->indegree = 0;
      s->outdegree = 0;
    }
}

/*----------------.
| Create a link.  |
`----------------*/

static symgraphlink *
symgraphlink_new (graphid id, symgraphlink *next)
{
  symgraphlink *l = xmalloc (sizeof *l);
  l->id = id;
  l->next = next;
  return l;
}

/*------------------------------------------------------------------.
| Register the second symbol of the precedence relation, and return |
| whether this relation is new.  Use only in register_precedence.   |
`------------------------------------------------------------------*/

static bool
register_precedence_second_symbol (symgraphlink **first, graphid sym)
{
  if (!*first || sym < (*first)->id)
    *first = symgraphlink_new (sym, *first);
  else
    {
      symgraphlink *slist = *first;

      while (slist->next && slist->next->id <= sym)
        slist = slist->next;

      if (slist->id == sym)
        /* Relation already present. */
        return false;

      slist->next = symgraphlink_new (sym, slist->next);
    }
  return true;
}

/*------------------------------------------------------------------.
| Register a new relation between symbols as used. The first symbol |
| has a greater precedence than the second one.                     |
`------------------------------------------------------------------*/

void
register_precedence (graphid first, graphid snd)
{
  if (!prec_nodes)
    init_prec_nodes ();
  if (register_precedence_second_symbol (&(prec_nodes[first]->succ), snd))
    prec_nodes[first]->outdegree += 1;
  if (register_precedence_second_symbol (&(prec_nodes[snd]->pred), first))
    prec_nodes[snd]->indegree += 1;
}

/*-------------------------.
| Free a symgraphlink list |
`-------------------------*/

static void
free_symgraphlink (symgraphlink *l)
{
  if (l)
    {
      free_symgraphlink (l->next);
      free (l);
    }
}

/*-----------------.
| Free a symgraph. |
`-----------------*/

static void free_symgraph (symgraph *s)
{
  if (s)
    {
      free_symgraphlink (s->pred);
      free_symgraphlink (s->succ);
      free (s);
    }
}

/*--------------------.
| Free symgraph list. |
`--------------------*/

static void
free_symgraph_list (symgraph *s)
{
  if (s)
    {
      free_symgraph_list (s->groupnext);
      free (s);
    }
}

/*--------------------------------------------------.
| Print a warning for unused precedence relations.  |
`--------------------------------------------------*/

void
print_precedence_warnings (void)
{
  int i;
  if (!prec_nodes)
    init_prec_nodes ();
  for (i = 0; i < nsyms; ++i)
    {
      symbol *s = symbols[i];
      if (s
          && s->prec != 0
          && !prec_nodes[i]->pred
          && !prec_nodes[i]->succ
          && s->assoc == precedence_assoc)
        complain (&s->location, Wprecedence,
                  _("useless precedence for %s"), s->tag);
    }
}

static intvect *
grow (intvect *vect, int size)
{
    if (!vect)
    {
        vect = malloc (sizeof (*vect));
        vect->size = size + 10;
        vect->t = xcalloc (size + 10, sizeof (int));
        return vect;
    }
    if (size <= vect->size)
        return vect;
    vect->t = xnrealloc (vect->t, size + 10, sizeof (int));
    for (int i = vect->size; i < size + 10; i += 1)
        vect->t[i] = 0;
    vect->size = size + 10;
    return vect;
}

/*---------------------------.
| Clone a symgraphlink list. |
`---------------------------*/

static symgraphlink *
copy_symgraphlink_list (symgraphlink *list)
{
  symgraphlink *l = 0;
  if (list)
    {
      l = malloc (sizeof (symgraphlink));
      l->id = list->id;
      l->next = copy_symgraphlink_list (list->next);
    }
  return l;
}

/*-----------------------------------------------------------------.
| Check if the element is in the group defined by the mark vector. |
`-----------------------------------------------------------------*/

static inline bool
is_precedence_in_group (intvect *mark, graphid el, int niter)
{
  return mark && mark->t[el] >= niter;
}

/*------------------------------------------------------------------------.
| Check whether two lists have the same element, ignoring those which are |
| in the same group.                                                      |
`------------------------------------------------------------------------*/

static bool
same_list (symgraphlink *l1, symgraphlink *l2, intvect *mark, int niter)
{
  if (l1 && is_precedence_in_group (mark, l1->id, niter))
    return same_list (l1->next, l2, mark, niter);
  if (l2 && is_precedence_in_group (mark, l2->id, niter))
    return same_list (l1, l2->next, mark, niter);
  if (l1 && l2 && l1->id == l2->id)
    return same_list (l1->next, l2->next, mark, niter);
  return !(l1 || l2);
}

/*--------------------------------------------------------------.
| Delete the link pointing to the element el in the list links. |
`--------------------------------------------------------------*/

static void
delete_one_link (symgraphlink **links, graphid el)
{
  if (el == (*links)->id)
    {
      symgraphlink *next = (*links)->next;
      free (*links);
      *links = next;
    }
  else
    for (symgraphlink *l = (*links); ; l = l->next)
      if (el == l->next->id)
        {
          symgraphlink *next = l->next;
          l->next = next->next;
          free (next);
          break;
        }
}

/*-----------------------------------------------------------------.
| Delete the links between the parent_node and the elements of the |
| newly-formed group, and add a link to the group instead.         |
`-----------------------------------------------------------------*/

static symgraphlink *
replace_links_one_node (symgraph *parent_node, symgraph *group, bool succ)
{
  symgraphlink **parent = &(parent_node->succ);
  if (!succ)
    parent = &(parent_node->pred);

  symgraph *el = group->symbols;
  for (symgraph *el = group->symbols; el; el = el->groupnext)
    {
      delete_one_link (parent, el->id);
      if (succ)
        parent_node->outdegree -= 1;
      else
        parent_node->indegree -= 1;
    }

  /* Lastly we add the group at the beginning. */
  if (succ)
    parent_node->outdegree += 1;
  else
    parent_node->indegree += 1;
  symgraphlink *newlink = xmalloc (sizeof(symgraphlink));
  newlink->id = group->id;
  newlink->next = *parent;
  *parent = newlink;
  return *parent;
}

/*----------------------------------------------.
| Remove links between the group and its nodes. |
`----------------------------------------------*/

static void clean_group_links (symgraph *group, intvect *mark, int niter)
{
  while (group->pred && is_precedence_in_group (mark, group->pred->id, niter))
    {
      symgraphlink *tmp = group->pred;
      group->pred = tmp->next;
      free (tmp);
    }
  if (group->pred)
    {
      symgraphlink *s = group->pred;
      while (s->next)
        {
          if (is_precedence_in_group (mark, s->next->id, niter))
            {
              symgraphlink *tmp = s->next;
              s->next = tmp->next;
              free (tmp);
              continue;
            }
          s = s->next;
        }
    }
  while (group->succ && is_precedence_in_group (mark, group->succ->id, niter))
    {
      symgraphlink *tmp = group->succ;
      group->succ = tmp->next;
      free (tmp);
    }
  if (group->succ)
    {
      symgraphlink *s = group->succ;
      while (s->next)
        {
          if (is_precedence_in_group (mark, s->next->id, niter))
            {
              symgraphlink *tmp = s->next;
              s->next = tmp->next;
              free (tmp);
              continue;
            }
          s = s->next;
        }
    }
}

/*--------------------------------------------------------------------------.
| Delete the links between nodes of a newly formed group and outside nodes. |
`--------------------------------------------------------------------------*/
static void
remove_out_links (symgraph *group)
{
  for (symgraph *s = group->symbols; s; s = s->groupnext)
    {
      for (symgraphlink *l = group->succ; l; l = l->next)
        {
          delete_one_link (&(s->succ), l->id);
          s->outdegree -= 1;
        }
      for (symgraphlink *l = group->pred; l; l = l->next)
        {
          delete_one_link (&(s->pred), l->id);
          s->indegree -= 1;
        }
    }
}

/*--------------------------------------------------------------------------.
| Remove the links between elements of a simple group (no intern links) and |
| outside nodes.                                                            |
`--------------------------------------------------------------------------*/
/*
static void
remove_nodes (symgraph *group)
{
  for (symgraph *s = group->symbols; s; s = s->groupnext)
    {
      free_symgraphlink (s->succ);
      free_symgraphlink (s->pred);
      s->succ = 0;
      s->pred = 0;
      s->outdegree = 0;
      s->indegree = 0;
    }
}
*/


/*-------------------------------------------------------------------------.
| Remove the links between the group's successors and predecessors and the |
| nodes of the group, and add a link to the group.                         |
`-------------------------------------------------------------------------*/

static symgraphlink *
replace_links (symgraph *group, graphid parentid, intvect *mark, int niter)
{
  /* The return value, a successor link to the group */
  symgraphlink *link_to_group = 0;
  symgraphlink *parent = group->pred;
  if (mark)
    clean_group_links (group, mark, niter);

  if (parent)
    {
      graphid index = parent->id;
      if (index == parentid)
        link_to_group = replace_links_one_node (prec_nodes[index], group, true);
      else
        replace_links_one_node (prec_nodes[index], group, true);
      for (; parent->next; parent = parent->next)
        {
          graphid index = parent->next->id;
          if(index == parentid)
            link_to_group = replace_links_one_node (prec_nodes[index], group, true);
          else
            replace_links_one_node (prec_nodes[index], group, true);
        }
    }

  parent = group->succ;
  if (parent)
    {
      replace_links_one_node (prec_nodes[parent->id], group, false);

      for (; parent->next; parent = parent->next)
        replace_links_one_node (prec_nodes[parent->next->id], group, false);
    }
  remove_out_links (group);

  return link_to_group;
}

/*---------------------------------------------------------.
| Add a symbol to a list of potential elements of a group. |
`---------------------------------------------------------*/

static symgraphlink *
add_to_potential (symgraphlink *potentialnodes, int id)
{
  symgraphlink *s = xmalloc (sizeof (*s));
  s->id = id;
  s->next = potentialnodes;
  return s;
}

/*---------------------------------------------------.
| Check if two symgraphlink lists are the same size. |
`---------------------------------------------------*/

static bool
same_size (symgraphlink *l1, symgraphlink *l2)
{
  if (l1 && l2)
    return same_size (l1->next, l2->next);
  return !(l1 || l2);
}

/*----------------------------------------------------------------------.
| Check among the list of elements in brothers if a complex group (with |
| links between internal elements) can be formed.                       |
`----------------------------------------------------------------------*/

static symgraphlink *
check_for_group (symgraph *ref, symgraphlink *brothers, intvect *mark, int niter)
{
  if (!brothers)
    return 0;
  symgraphlink *potentialnodes = 0;
  for (symgraphlink *l = brothers; l; l = l->next)
    {
      symgraph *s = prec_nodes[l->id];
      if ((mark || !(markvector->t[s->id] & 2)) &&
          same_list (ref->succ, s->succ, mark, niter) &&
          same_list (ref->pred, s->pred, mark, niter))
        {
          potentialnodes = add_to_potential (potentialnodes, l->id);
          if (mark)
            mark->t[l->id] = niter + 1;
        }
    }

  if (!same_size (brothers, potentialnodes))
    {
      if (niter != 1)
        free_symgraphlink (brothers);
      return check_for_group (ref, potentialnodes, mark, niter + 1);
    }
  return potentialnodes;
}

/*-----------------------------------------------.
| Create a group with the elements node and sym. |
`-----------------------------------------------*/

static symgraph *
create_group (symgraph *node, symgraphlink *sym, int *gcreated, intvect **mark)
{
  symgraph *group = xmalloc (sizeof (*group));
  group->id = nsyms + ngroups + *gcreated;
  group->symbols = node;
  group->pred = copy_symgraphlink_list (node->pred);
  group->succ = copy_symgraphlink_list (node->succ);
  group->groupnext = 0;
  group->outdegree = node->outdegree;
  group->indegree = node->indegree;
  *gcreated += 1;
  markvector = grow (markvector, group->id + 1);
  markvector->t[group->id] = 0;
  if (*mark)
    {
      *mark = grow (*mark, group->id + 1);
      (*mark)->t[group->id] = 0;
    }
  prec_nodes = xnrealloc (prec_nodes, group->id + 1, sizeof(symgraph *));
  prec_nodes[group->id] = group;

  for (symgraph *cur = group->symbols; sym; sym = sym->next)
    {
      symgraph *s = prec_nodes[sym->id];
      cur->groupnext = s;
      cur = s;
    }

  free_symgraphlink (sym);
  return group;
}

/*---------------------------------------------------------------------------.
| Attempt to from a group from the successors of a node.                     |
| If in_links, then try to make a group with possible links between nodes in |
| the group.                                                                 |
`---------------------------------------------------------------------------*/

static void
depth_grouping (symgraph *node, int *gcreated, bool in_links)
{
  if (markvector->t[node->id] & 1)
    return;
  markvector->t[node->id] |= 1;

  /* Mark array to check whether a node belongs to the group being formed. */
  intvect *mark_intern = 0;
  int dim = nsyms + ngroups + *gcreated;
  if (in_links)
    {
      mark_intern = grow (mark_intern, dim);
      for (symgraphlink *l = node->succ; l; l = l->next)
        mark_intern->t[l->id] = 1;
    }

  /* Go through the successors. */
  for (symgraphlink *linkson = node->succ; linkson; linkson = linkson->next)
    {
      graphid markindex = linkson->id;
      symgraph *son = prec_nodes[markindex];
      if (!in_links && markvector->t[markindex] & 2)
        continue;

      markvector->t[markindex] |= 2;
      if (in_links)
        mark_intern->t[son->id] = 2;

      symgraphlink *groupnodes = check_for_group (son, linkson->next,
                                                  mark_intern, 1);

      /* If there is a non-trivial equivalence class, create a group. */
      if (groupnodes)
        {
          symgraph *group = create_group (son, groupnodes, gcreated,
                                          &mark_intern);

          int niter = mark_intern ? mark_intern->t[groupnodes->id]  : 0;
          symgraphlink *tmp = replace_links (group, node->id,
                                             mark_intern, niter);
          if(tmp)
            linkson = tmp;

          /* Reset the vector */
          if (in_links)
            for (graphid i = 0; i < dim; i += 1)
              mark_intern->t[i] &= 1;
        }
    }

  if (in_links)
    {
      free (mark_intern->t);
      free (mark_intern);
    }

  for (symgraphlink *link = node->succ; link; link= link->next)
    {
      symgraph *son = prec_nodes[link->id];
      depth_grouping (son, gcreated, in_links);
    }
}

/*---------------------------------------------------------.
| Create a virtual node pointing to the roots of the graph |
`---------------------------------------------------------*/

static symgraph *
get_virtual_root (void)
{
  symgraph *root = prec_nodes[0];
  root->succ = 0;
  root->outdegree = 0;
  for (graphid i = 1; i < nsyms + ngroups; ++i)
    {
      symgraph *s = prec_nodes[i];
      if ((s->pred == 0 || s->pred && s->pred->id == 0) && s->succ != 0)
        {
          symgraphlink *l = symgraphlink_new (i, root->succ);
          root->succ = l;
          root->outdegree += 1;

          l = symgraphlink_new (0, s->pred);
          s->pred = l;
          s->indegree += 1;
        }
    }
  return root;
}

/*--------------------------------------------------------------------.
| Group the nodes of the graph if they have the same predecessors and |
| successors, and in a second time, if they also have links between   |
| other nodes of the group.                                           |
`--------------------------------------------------------------------*/

static void
group_relations (void)
{
  ngroups = 0;
  markvector = grow (markvector, nsyms);

  /* Number of group created during the current pass. */
  int *gcreated = xmalloc (sizeof(int));
  symgraph *root = get_virtual_root();

  symgraph *beginning = root;
  *gcreated = 0;
  depth_grouping (root, gcreated, false);
  ngroups += *gcreated;

  return;
  do
    {
      for (int i = 0; i < nsyms + ngroups; i += 1)
        markvector->t[i] = 0;

      root = get_virtual_root();
      beginning = root;
      *gcreated = 0;
      depth_grouping (root, gcreated, true);

      ngroups += *gcreated;
    } while (*gcreated > 0);

  free(gcreated);
}

/*---------------------------------------.
| Initialize association tracking table. |
`---------------------------------------*/

static void
init_assoc (void)
{
  graphid i;
  used_assoc = xcalloc(nsyms, sizeof(*used_assoc));
  for (i = 0; i < nsyms; ++i)
    used_assoc[i] = false;
}

/*---------------------------------------------------.
| Check if the symgraph is a group or a single node. |
`---------------------------------------------------*/

static inline bool is_group (symgraph *s)
{
  return s->id >= nsyms;
}

/*-----------------------------------------------------------------------.
| Output the dot declaration of the node if it hasn't already been done. |
`-----------------------------------------------------------------------*/

static void
declare_symbol_graph (FILE *f, graphid index, bool *mark)
{
  if (mark[index])
    return;
  mark[index] = 1;
  symgraph *sym = prec_nodes[index];
  if (is_group (sym))
    {
      fprintf (f, "subgraph cluster_%i {\n", index);
      symgraph *el = sym->symbols;
      while (el)
        {
          declare_symbol_graph (f, el->id, mark);
          el = el->groupnext;
        }
      fprintf (f, "}\n");
    }
  else
    fprintf (f, "%i [label=\"%s\"]\n", sym->id,
             symbols[sym->id]->tag);
}

/*---------------------------------------------------------------------------.
| Get the first actual node (not group) of a group, to have a valid starting |
| or arrival point in the dot file.                                          |
`---------------------------------------------------------------------------*/

static int
get_first_symbol (symgraph *graph)
{
  while (is_group (graph))
    graph = graph->symbols;
  return graph->id;
}

/*----------------------------------------------.
| Print a link between two nodes to a dot file. |
`----------------------------------------------*/

static void
print_graph_link (FILE *f, graphid tail, graphid head, bool col)
{
  symgraph *tsym = prec_nodes[tail], *hsym = prec_nodes[head];

  char *color = "black";
  if (col)
    {
      if (tsym->outdegree == 1)
        {
          if (hsym->indegree == 1)
            color = "red";
          else
            color = "blue";
        }
      else if (hsym->indegree == 1)
        color = "red";
    }
  fprintf (f, "%i -> %i [", get_first_symbol (tsym), get_first_symbol (hsym));
  if (is_group (hsym))
    {
      if (is_group (tsym))
        fprintf (f, "lhead=cluster_%i, ltail=cluster_%i, ", head, tail);
      else
        fprintf (f, "lhead=cluster_%i, ", head);
    }
  else if (is_group (tsym))
    fprintf (f, "ltail=cluster_%i, ", tail);
  fprintf (f, "color=%s];\n", color);
}

/*--------------------------------------.
| Creates the used relations dot graph. |
`--------------------------------------*/

void
print_rel_dot_graph (FILE *f)
{
  group_relations ();
  fprintf (f, "digraph rel{\ncompound=true; nodesep=\"0.3 equally\";"
           "ranksep=\"3 equally\";\nsubgraph cluster_legend { \n"
           "label=legend\n\"outdegree=1\" -> \"indegree<>1\""
           " [color=blue];\n\"outdegree=1\" -> \"indegree=1\" "
           "[color=red];\n\"outdegree<>1\" -> \"indegree=1\" "
           "[color=green];\n}\n");
  bool mark[nsyms + ngroups];
  for (graphid i = 0; i < nsyms + ngroups; i += 1)
    mark[i] = false;

  /* Loop backwards because the groups have to be declared before their
   * elements. */
  for (graphid i = nsyms + ngroups - 1; i > 0; i -= 1)
    {
      symgraph *symgraph = prec_nodes[i];
      if (!symgraph->succ && !symgraph->pred)
        continue;
      graphid gid = symgraph->id;
      declare_symbol_graph (f, gid, mark);
      for (symgraphlink *slink = symgraph->succ; slink; slink = slink->next)
        {
          graphid lid = slink->id;
          declare_symbol_graph (f, lid, mark);
          print_graph_link (f, gid, lid, true);
        }
    }
  fprintf (f, "}");
}

/*------------------------------------------------------------------.
| Test if the associativity for the symbols is defined and useless. |
`------------------------------------------------------------------*/

static inline bool
is_assoc_useless (symbol *s)
{
  return s
      && s->assoc != undef_assoc
      && s->assoc != precedence_assoc
      && !used_assoc[s->number];
}

/*-------------------------------.
| Register a used associativity. |
`-------------------------------*/

void
register_assoc (graphid i, graphid j)
{
  if (!used_assoc)
    init_assoc ();
  used_assoc[i] = true;
  used_assoc[j] = true;
}

/*------------------------------------------------------.
| Print a warning for each unused symbol associativity. |
`------------------------------------------------------*/

void
print_assoc_warnings (void)
{
  graphid i;
  if (!used_assoc)
    init_assoc ();
  for (i = 0; i < nsyms; ++i)
    {
      symbol *s = symbols[i];
      if (is_assoc_useless (s))
        complain (&s->location, Wprecedence,
                  _("useless associativity for %s"), s->tag);
    }
}

/*------------------------------.
| Create a new n*n bool matrix. |
`------------------------------*/

static bool **
new_graph (graphid n)
{
  bool **g = xnmalloc (n, sizeof (*g));
  for (graphid i = 0; i < n; i += 1)
      g[i] = xcalloc (n, sizeof (**g));
  return g;
}

/*----------------------------------------.
| Convert the graph in nodes to a matrix. |
`----------------------------------------*/

static bool **
dynamic_graph_to_matrix (void)
{
  bool **g = new_graph (nsyms + ngroups);
  for (graphid i = 0; i < nsyms + ngroups; i += 1)
    {
      symgraph *s = prec_nodes[i];
      for (symgraphlink *l = s->succ; l; l = l->next)
        g[i][l->id] = true;
    }
  return g;
}

/*-----------------------------.
| Return a copy of the matrix. |
`-----------------------------*/

static bool **
copy_graph (bool **c)
{
  graphid n = nsyms + ngroups;
  bool **g = xnmalloc (n, sizeof (*g));
  for (graphid i = 0; i < n; i += 1)
    {
      g[i] = xnmalloc (n, sizeof (**g));
      for (graphid j = 0; j < n; j += 1)
        g[i][j] = c[i][j];
    }
  return g;
}

/*-------------------------------------------.
| Compute the transitive closure of a graph. |
`-------------------------------------------*/

static bool **
transitive_closure (bool ** g)
{
  bool **cl = copy_graph (g);
  for (int k = 0; k < nsyms + ngroups; k += 1)
    for (int i = 0; i < nsyms + ngroups; i += 1)
      for (int j = 0; j < nsyms + ngroups; j += 1)
        if (cl[i][k] && cl[k][j])
          cl[i][j] = true;
  return cl;
}

/*---------------------------------------------.
| Compute the transitive reduction of a graph. |
`---------------------------------------------*/

static void
transitive_reduction (bool **g)
{
  bool **cl = transitive_closure (g);
  bool **rem = new_graph (nsyms + ngroups);

  for (int i = 0; i < nsyms + ngroups; i += 1)
    {
      for (int j = 0; j < nsyms + ngroups; j += 1)
        {
          if (!cl[i][j])
            continue;
          for (int k = 0; k < nsyms + ngroups; k += 1)
            {
              if (!cl[j][k])
                continue;
              rem[i][k] = true;
            }
        }
    }

  for (int i = 0; i < nsyms + ngroups; i += 1)
    {
      for (int j = 0; j < nsyms + ngroups; j += 1)
        g[i][j] = cl[i][j] && !rem[i][j];
      free (rem[i]);
      free (cl[i]);
    }

  free (rem);
  free (cl);
}

/*-------------------------------------------------------------.
| Print the transitive reduction of the nodes graph to a file. |
`-------------------------------------------------------------*/

void
print_transitive_reduction (FILE *f)
{
  bool **g = dynamic_graph_to_matrix ();
  transitive_reduction (g);
  fprintf (f, "digraph rel{\ncompound=true; nodesep=\"0.3 equally\";"
           "ranksep=\"3 equally\";\n");
  bool mark[nsyms + ngroups];
  for (graphid i = 0; i < nsyms + ngroups; i += 1)
    mark[i] = false;

  for (int i = 0; i < nsyms + ngroups; i += 1)
    {

      for (int j = 0; j < nsyms + ngroups; j += 1)
        {
          if (!g[i][j])
            continue;
          declare_symbol_graph (f, i, mark);
          declare_symbol_graph (f, j, mark);
          print_graph_link (f, i, j, false);
        }
      free (g[i]);
    }
  fprintf (f, "}");
  free (g);
}
