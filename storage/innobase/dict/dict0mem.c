/*****************************************************************************

Copyright (c) 1996, 2010, Innobase Oy. All Rights Reserved.

This program is free software; you can redistribute it and/or modify it under
the terms of the GNU General Public License as published by the Free Software
Foundation; version 2 of the License.

This program is distributed in the hope that it will be useful, but WITHOUT
ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License along with
this program; if not, write to the Free Software Foundation, Inc., 59 Temple
Place, Suite 330, Boston, MA 02111-1307 USA

*****************************************************************************/

/******************************************************************//**
@file dict/dict0mem.c
Data dictionary memory object creation

Created 1/8/1996 Heikki Tuuri
***********************************************************************/

#include "dict0mem.h"

#ifdef UNIV_NONINL
#include "dict0mem.ic"
#endif

#include "rem0rec.h"
#include "data0type.h"
#include "mach0data.h"
#include "dict0dict.h"
#include "ha_prototypes.h" /* innobase_casedn_str()*/
#ifndef UNIV_HOTBACKUP
# include "lock0lock.h"
#endif /* !UNIV_HOTBACKUP */
#ifdef UNIV_BLOB_DEBUG
# include "ut0rbt.h"
#endif /* UNIV_BLOB_DEBUG */

#define	DICT_HEAP_SIZE		100	/*!< initial memory heap size when
					creating a table or index object */

#define REALLOC_COUNT(init_val,step,val)  (val)<=(init_val)? (init_val):((init_val) + (unsigned)ceil((((double)(val) - (init_val))/(step)))*(step))

#ifdef UNIV_PFS_MUTEX
/* Key to register autoinc_mutex with performance schema */
UNIV_INTERN mysql_pfs_key_t	autoinc_mutex_key;
#endif /* UNIV_PFS_MUTEX */

/**********************************************************************//**
Creates a table memory object.
@return	own: table object */
UNIV_INTERN
dict_table_t*
dict_mem_table_create(
/*==================*/
	const char*	name,	/*!< in: table name */
	ulint		space,	/*!< in: space where the clustered index of
				the table is placed; this parameter is
				ignored if the table is made a member of
				a cluster */
	ulint		n_cols,	/*!< in: number of columns */
	ulint		flags,	/*!< in: table flags */
    ibool       is_gcs, /*!< in: gcs table flag */
    ulint       n_cols_before_alter     /*!< in: number of columns before gcs table alter table */
)
{
	dict_table_t*	table;
	mem_heap_t*	heap;

	ut_ad(name);
	ut_a(!(flags & (~0 << DICT_TF2_BITS)));
    ut_ad(!is_gcs || flags & DICT_TF_COMPACT);              /* GCS必须是compact格式 */

    ut_a(n_cols_before_alter <= n_cols);

	heap = mem_heap_create(DICT_HEAP_SIZE);

	table = mem_heap_zalloc(heap, sizeof(dict_table_t));

	table->heap = heap;

	table->flags = (unsigned int) flags;
	table->name = ut_malloc(strlen(name) + 1);
	memcpy(table->name, name, strlen(name) + 1);
	table->space = (unsigned int) space;
	table->n_cols = (unsigned int) (n_cols + DATA_N_SYS_COLS);
    
    /* init n_cols_allocated as 0 */
    table->n_cols_allocated = 0;
    table->col_names_length_alloced = 0;

	/*table->cols = mem_heap_alloc(heap, (n_cols + DATA_N_SYS_COLS)
				     * sizeof(dict_col_t));*/
    table->cols = dict_mem_realloc_dict_cols(table,(n_cols + DATA_N_SYS_COLS));

#ifndef UNIV_HOTBACKUP
	table->autoinc_lock = mem_heap_alloc(heap, lock_get_size());

	mutex_create(autoinc_mutex_key,
		     &table->autoinc_mutex, SYNC_DICT_AUTOINC_MUTEX);

	table->autoinc = 0;
    table->is_gcs  = is_gcs;
    ut_ad(!n_cols_before_alter || is_gcs);      /* 仅对gcs表有效，其他表必为0 */
    if (n_cols_before_alter > 0)
        table->n_cols_before_alter_table = (ulint)(n_cols_before_alter +  DATA_N_SYS_COLS);
    else
        table->n_cols_before_alter_table = 0;

	/* The number of transactions that are either waiting on the
	AUTOINC lock or have been granted the lock. */
	table->n_waiting_or_granted_auto_inc_locks = 0;
#endif /* !UNIV_HOTBACKUP */

	ut_d(table->magic_n = DICT_TABLE_MAGIC_N);
	return(table);
}

/****************************************************************//**
Free a table memory object. */
UNIV_INTERN
void
dict_mem_table_free(
/*================*/
	dict_table_t*	table)		/*!< in: table */
{
	ut_ad(table);
	ut_ad(table->magic_n == DICT_TABLE_MAGIC_N);
	ut_d(table->cached = FALSE);

#ifndef UNIV_HOTBACKUP
	mutex_free(&(table->autoinc_mutex));
#endif /* UNIV_HOTBACKUP */
	ut_free(table->name);
    ut_free(table->cols);
    ut_free(table->col_names);
	mem_heap_free(table->heap);
}

/****************************************************************//**
Append 'name' to 'col_names'.  @see dict_table_t::col_names
@return	new column names array */
static
char*
dict_add_col_name(
/*==============*/
	dict_table_t* table,	/*!< in: dict table, or NULL */
	ulint		cols,		/*!< in: number of existing columns */
	const char*	name,		/*!< in: new column name */
	mem_heap_t*	heap)		/*!< in: heap */
{
	ulint	old_len;
	ulint	new_len;
	ulint	total_len;
	char*	res;
    char*	col_names = table->col_names;

	ut_ad(!cols == !col_names);

	/* Find out length of existing array. */
	if (col_names) {
		const char*	s = col_names;
		ulint		i;

		for (i = 0; i < cols; i++) {
			s += strlen(s) + 1;
		}

		old_len = s - col_names;
	} else {
		old_len = 0;
	}


	new_len = strlen(name) + 1;
	total_len = old_len + new_len;

	//res = mem_heap_alloc(heap, total_len);
    res = dict_mem_realloc_dict_col_names(table,total_len);

	//if (old_len > 0) {
	//	memcpy(res, col_names, old_len);
	//}

	memcpy(res + old_len, name, new_len);

	return(res);
}

/**********************************************************************//**
Adds a column definition to a table. */
UNIV_INTERN
void
dict_mem_table_add_col(
/*===================*/
	dict_table_t*	table,	/*!< in: table */
	mem_heap_t*	heap,	/*!< in: temporary memory heap, or NULL */
	const char*	name,	/*!< in: column name, or NULL */
	ulint		mtype,	/*!< in: main datatype */
	ulint		prtype,	/*!< in: precise type, maybe 29 bit bolb compress */
	ulint		len)	/*!< in: precision */
{
	dict_col_t*	col;
	ulint		i;

	ut_ad(table);
	ut_ad(table->magic_n == DICT_TABLE_MAGIC_N);
	//ut_ad(!heap == !name);

	i = table->n_def++;

	if (name) {
		if (UNIV_UNLIKELY(table->n_def == table->n_cols)) {
			heap = table->heap;
		}
		if (UNIV_LIKELY(i) && UNIV_UNLIKELY(!table->col_names)) {
			/* All preceding column names are empty. */
			//char* s = mem_heap_zalloc(heap, table->n_def);
			//table->col_names = s;
            table->col_names = dict_mem_realloc_dict_col_names(table,table->n_def);
		}

		table->col_names = dict_add_col_name(table,i, name, heap);
	}

	col = dict_table_get_nth_col(table, i);

	dict_mem_fill_column_struct(col, i, mtype, prtype, len);
}


//void
//dict_mem_table_add_col_default_low(
//   dict_col_t*          col                                    
//)
//{
//    ut_ad(col->def_val);
//    switch (col->mtype)
//    {
//    case DATA_VARCHAR:
//    case DATA_CHAR:
//    case DATA_BINARY:
//    case DATA_FIXBINARY:
//    case DATA_BLOB:
//    case DATA_MYSQL:
//    case DATA_DECIMAL:
//    case DATA_VARMYSQL:
//        col->def_val->real_val.var_val = col->def_val->def_val;
//        break;
//
//    case DATA_INT:
//        /*
//        TODO(GCS): int maybe in 1-4 byte.
//        */
//       // ut_ad(col->def_val->def_val_len == 4);
//      //  col->def_val->real_val.int_val = mach_read_from_4(col->def_val->def_val);
//        break;
//
//    case DATA_FLOAT:
//        ut_ad(col->def_val->def_val_len == sizeof(float));
//        col->def_val->real_val.float_val = mach_float_read(col->def_val->def_val);
//        break;
//
//    case DATA_DOUBLE:
//        ut_ad(col->def_val->def_val_len == sizeof(double));
//        col->def_val->real_val.double_val = mach_double_read(col->def_val->def_val);
//        break;
//
//    case DATA_SYS_CHILD:	/* address of the child page in node pointer */
//    case DATA_SYS:          /* system column */
//    default:
//        ut_a(0);
//        break;
//    }
//}

UNIV_INTERN
void
dict_mem_table_add_col_default(
    dict_table_t*           table,             /* 注意，此table可能不可靠，此时table可能并没加入该列信息 */
    dict_col_t*             col,
    mem_heap_t*             heap,
    const byte*             def_val,
    ulint                   def_val_len
)
{
    ut_ad(table && col && heap && def_val && def_val_len >= 0);
    ut_ad(!dict_col_is_nullable(col));

#ifdef UNIV_DEBUG
    {
        ulint               fixed_size;
        fixed_size = dict_col_get_fixed_size(col, 1);
        ut_ad(fixed_size == 0 || fixed_size == def_val_len);
    }
#endif // _DEBUG

    
    col->def_val = mem_heap_alloc(heap, sizeof(*col->def_val));
    col->def_val->col = col;
    col->def_val->def_val_len = def_val_len;
    col->def_val->def_val = (byte*)mem_heap_strdupl(heap, (char*)def_val, def_val_len);

   // dict_mem_table_add_col_default_low(col);
}

/* 设置列的默认值，字符型内容为空格，其他都是0x00, 用于redo */
UNIV_INTERN
void
dict_mem_table_set_col_default(
    dict_table_t*           table,
    dict_col_t*             col,
    mem_heap_t*             heap
)
{
    ulint       fixed_size = 0;
    ulint       def_val_len = 0;

    ut_ad(!dict_col_is_nullable(col));
    ut_ad(dict_table_is_gcs_after_alter_table(table));

    fixed_size = dict_col_get_fixed_size(col, dict_table_is_comp(table));

    /* 变长字段只用一个字节 */
    def_val_len = fixed_size ? fixed_size : 1;

    col->def_val = mem_heap_alloc(heap, sizeof(*col->def_val));
    col->def_val->col = col;
    col->def_val->def_val_len = def_val_len;
    /* 分配空间，同时初始化为0 */
    col->def_val->def_val = mem_heap_zalloc(heap, def_val_len + 1);

    /* 对于字符类型，默认都是空格 */
    if(dtype_is_string_type(col->mtype)) 
        memset(col->def_val->def_val, ' ', def_val_len);

   // dict_mem_table_add_col_default_low(col);

}
/**********************************************************************//**
This function populates a dict_col_t memory structure with
supplied information. */
UNIV_INTERN
void
dict_mem_fill_column_struct(
/*========================*/
	dict_col_t*	column,		/*!< out: column struct to be
					filled */
	ulint		col_pos,	/*!< in: column position */
	ulint		mtype,		/*!< in: main data type */
	ulint		prtype,		/*!< in: precise type */
	ulint		col_len)	/*!< in: column length */
{
#ifndef UNIV_HOTBACKUP
	ulint	mbminlen;
	ulint	mbmaxlen;
#endif /* !UNIV_HOTBACKUP */

    /* only blob field have compressed  */
	ut_ad(mtype == DATA_BLOB || !(prtype & DATA_IS_BLOB_COMPRESSED));

	column->ind = (unsigned int) col_pos;
	column->ord_part = 0;
	column->max_prefix = 0;
	column->mtype = (unsigned int) mtype;
	column->prtype = (unsigned int) prtype;
	column->len = (unsigned int) col_len;
    column->def_val = NULL;
#ifndef UNIV_HOTBACKUP
        dtype_get_mblen(mtype, prtype, &mbminlen, &mbmaxlen);
	dict_col_set_mbminmaxlen(column, mbminlen, mbmaxlen);
#endif /* !UNIV_HOTBACKUP */
}

/**********************************************************************//**
Creates an index memory object.
@return	own: index object */
UNIV_INTERN
dict_index_t*
dict_mem_index_create(
/*==================*/
	const char*	table_name,	/*!< in: table name */
	const char*	index_name,	/*!< in: index name */
	ulint		space,		/*!< in: space where the index tree is
					placed, ignored if the index is of
					the clustered type */
	ulint		type,		/*!< in: DICT_UNIQUE,
					DICT_CLUSTERED, ... ORed */
	ulint		n_fields)	/*!< in: number of fields */
{
	dict_index_t*	index;
	mem_heap_t*	heap;

	ut_ad(table_name && index_name);

	heap = mem_heap_create(DICT_HEAP_SIZE);
	index = mem_heap_zalloc(heap, sizeof(dict_index_t));

    /* new index,set n_fields_allocated ZERO */
    index->n_fields_allocated = 0;
	dict_mem_fill_index_struct(index, heap, table_name, index_name,
				   space, type, n_fields);

	return(index);
}

/**********************************************************************//**
Creates and initializes a foreign constraint memory object.
@return	own: foreign constraint struct */
UNIV_INTERN
dict_foreign_t*
dict_mem_foreign_create(void)
/*=========================*/
{
	dict_foreign_t*	foreign;
	mem_heap_t*	heap;

	heap = mem_heap_create(100);

	foreign = mem_heap_zalloc(heap, sizeof(dict_foreign_t));

	foreign->heap = heap;

	return(foreign);
}

/**********************************************************************//**
Sets the foreign_table_name_lookup pointer based on the value of
lower_case_table_names.  If that is 0 or 1, foreign_table_name_lookup
will point to foreign_table_name.  If 2, then another string is
allocated from foreign->heap and set to lower case. */
UNIV_INTERN
void
dict_mem_foreign_table_name_lookup_set(
/*===================================*/
	dict_foreign_t*	foreign,	/*!< in/out: foreign struct */
	ibool		do_alloc)	/*!< in: is an alloc needed */
{
	if (innobase_get_lower_case_table_names() == 2) {
		if (do_alloc) {
			foreign->foreign_table_name_lookup = mem_heap_alloc(
				foreign->heap,
				strlen(foreign->foreign_table_name) + 1);
		}
		strcpy(foreign->foreign_table_name_lookup,
		       foreign->foreign_table_name);
		innobase_casedn_str(foreign->foreign_table_name_lookup);
	} else {
		foreign->foreign_table_name_lookup
			= foreign->foreign_table_name;
	}
}

/**********************************************************************//**
Sets the referenced_table_name_lookup pointer based on the value of
lower_case_table_names.  If that is 0 or 1, referenced_table_name_lookup
will point to referenced_table_name.  If 2, then another string is
allocated from foreign->heap and set to lower case. */
UNIV_INTERN
void
dict_mem_referenced_table_name_lookup_set(
/*======================================*/
	dict_foreign_t*	foreign,	/*!< in/out: foreign struct */
	ibool		do_alloc)	/*!< in: is an alloc needed */
{
	if (innobase_get_lower_case_table_names() == 2) {
		if (do_alloc) {
			foreign->referenced_table_name_lookup = mem_heap_alloc(
				foreign->heap,
				strlen(foreign->referenced_table_name) + 1);
		}
		strcpy(foreign->referenced_table_name_lookup,
		       foreign->referenced_table_name);
		innobase_casedn_str(foreign->referenced_table_name_lookup);
	} else {
		foreign->referenced_table_name_lookup
			= foreign->referenced_table_name;
	}
}

/**********************************************************************//**
Adds a field definition to an index. NOTE: does not take a copy
of the column name if the field is a column. The memory occupied
by the column name may be released only after publishing the index. */
UNIV_INTERN
void
dict_mem_index_add_field(
/*=====================*/
	dict_index_t*	index,		/*!< in: index */
	const char*	name,		/*!< in: column name */
	ulint		prefix_len)	/*!< in: 0 or the column prefix length
					in a MySQL index like
					INDEX (textcol(25)) */
{
	dict_field_t*	field;

	ut_ad(index);
	ut_ad(index->magic_n == DICT_INDEX_MAGIC_N);

	index->n_def++;

	field = dict_index_get_nth_field(index, index->n_def - 1);

	field->name = name;
	field->prefix_len = (unsigned int) prefix_len;
}

/**********************************************************************//**
Frees an index memory object. */
UNIV_INTERN
void
dict_mem_index_free(
/*================*/
	dict_index_t*	index)	/*!< in: index */
{
	ut_ad(index);
	ut_ad(index->magic_n == DICT_INDEX_MAGIC_N);
#ifdef UNIV_BLOB_DEBUG
	if (index->blobs) {
		mutex_free(&index->blobs_mutex);
		rbt_free(index->blobs);
	}
#endif /* UNIV_BLOB_DEBUG */

    ut_free(index->fields);
	mem_heap_free(index->heap);
}


/**********************************************************************//**
直接对字典对象内存增加若干列 
*/
void
dict_mem_table_add_col_simple(
    dict_table_t*           table,              /*!< in: 原表字典对象 */
    dict_col_t*             col_arr,            /*!< in: 增加列后的用户列字典对象,不包含系统列 */
    ulint                   n_col,              /*!< in: 新增列后表列数量,不包含系统列 */
    char*                   col_names,          /*!< in: 用户新列所有列名 */
    ulint                   col_names_len       /*!< in: col_names的长度 */
)
{
    ulint                   org_heap_size = 0;    
    ulint                   org_n_cols = table->n_def - DATA_N_SYS_COLS;
    ulint                   add_n_cols = n_col - org_n_cols;
    dict_index_t*           index = NULL;
    dict_index_t*           clu_index = NULL;
    dict_field_t*           fields;
    ulint                   i;
	ibool					first_alter = FALSE; 

    ut_ad(dict_table_is_gcs(table) && table->cached);
    ut_ad(table->n_def < n_col + DATA_N_SYS_COLS && table->n_def  == table->n_cols);

    org_heap_size = mem_heap_get_size(table->heap);

    if (table->n_cols_before_alter_table == 0)
    {
		first_alter = TRUE;
        /* 若第一次alter table add column，修改该值 */
        table->n_cols_before_alter_table = table->n_cols;
    }

    /* 拷贝前N列（用户列）及列名 */
    /*table->cols = mem_heap_zalloc(table->heap, sizeof(dict_col_t) * (DATA_N_SYS_COLS + n_col));*/
    table->cols = dict_mem_realloc_dict_cols(table,(n_col + DATA_N_SYS_COLS));
    memcpy(table->cols, col_arr, n_col * sizeof(dict_col_t));

    
    /* 
    还原默认值信息，使用table->heap分配内存
    org_n_cols之前的列都已经分配默认值空间了,因此只需要分配本次增加的列默认值即可(重复加字段可以省很多内存) 
    */
    for (i = org_n_cols; i < n_col; ++i)
    {
        if (table->cols[i].def_val)
        {
            dict_mem_table_add_col_default(table, &table->cols[i], table->heap,table->cols[i].def_val->def_val, (ulint)table->cols[i].def_val->def_val_len);
        }
    }

    table->n_def = n_col;
    table->n_cols = n_col + DATA_N_SYS_COLS;
    
    table->col_names = dict_mem_realloc_dict_col_names(table,col_names_len);    
    memcpy(table->col_names, col_names, col_names_len);
  
    /* 增加系统列 */
    table->cached = FALSE;          /* 避免断言 */
    /* 内存分配修改为直接从系统分配,不从heap分配,此处传入NULL值 */
    dict_table_add_system_columns(table, NULL);
    table->cached = TRUE;

    dict_table_set_big_row(table);   

    dict_sys->size -= org_heap_size;
    dict_sys->size += mem_heap_get_size(table->heap);

    /* 更新索引及索引列信息 */
    index = UT_LIST_GET_FIRST(table->indexes);
    while (index)
    {
        ulint               n_fields;    
        ibool               is_gen_clust_index = FALSE;       

        ut_ad(index->n_def == index->n_fields);

        org_heap_size = mem_heap_get_size(index->heap);        

        if (dict_index_is_clust(index))
        {
            clu_index = index;

            /* 修改主键为rowid的ord_part值 */
            if (!strcmp(clu_index->name, "GEN_CLUST_INDEX"))
            {
                is_gen_clust_index = TRUE;
            }            

            ut_ad(index->n_fields <= index->n_user_defined_cols + table->n_cols);

            index->fields = dict_mem_realloc_index_fields(index,index->n_user_defined_cols + table->n_cols);
            
            /* 聚集索引需要增加最后几列的信息 */
            for (i = 0; i < add_n_cols; ++i)
            {
                dict_index_add_col(index, table, dict_table_get_nth_col(table, org_n_cols + i), 0);  /* n_nullable已在dict_index_add_col处理 */
            }

            /* 聚集索引只处理前n_field列，因新增的若干列已经在上面处理了 */
            n_fields = index->n_fields;

            index->n_fields     += add_n_cols;
            ut_ad(index->n_fields == index->n_def);

			/* 记录第一次加字段前的字段信息 */
            if (index->n_fields_before_alter == 0)
            {
                ut_ad(first_alter);               
                index->n_fields_before_alter = index->n_fields - add_n_cols;
                
                index->n_nullable_before_alter = dict_index_get_first_n_field_n_nullable(index, index->n_fields_before_alter);
            }
            
        }
        else
        {
            ut_ad(index->n_user_defined_cols + clu_index->n_uniq + 1 >= index->n_fields);
            ut_ad(clu_index != NULL);
            n_fields = index->n_fields;
        }

        fields = index->fields;       
        
        /* 修改索引列col和name指针地址 */
        for (i = 0; i < n_fields; ++i)
        {         
            ulint       col_ind = fields[i].col_ind;

            /* 因为表中org_n_cols后插入了若干列，原列索引大于等于org_n_cols(系统列)都需要增大增加的列数 */
            if (col_ind >= org_n_cols)
                col_ind += add_n_cols;

            fields[i].col = dict_table_get_nth_col(table, col_ind);
            fields[i].col_ind = fields[i].col->ind;
            fields[i].name = dict_table_get_col_name(table, col_ind);

            if (is_gen_clust_index && !strcmp(fields[i].name, "DB_ROW_ID"))
            {
                ut_ad(!fields[i].col->ord_part);
                fields[i].col->ord_part = 1;
            }
            
        }

        dict_sys->size -= org_heap_size;
        dict_sys->size += mem_heap_get_size(index->heap);

        index = UT_LIST_GET_NEXT(indexes, index);
    }
  
    /* 外键 */
}



/**********************************************************************//**
直接对字典对象内存删除采用快速加字段新增的列 
*/
void
dict_mem_table_drop_col_simple(
    dict_table_t*           table,              /*!< in: 表字典对象,已快速加过字段 */
    dict_col_t*             col_arr,            /*!< in: 删除列后的用户列字典对象 */
    ulint                   n_col,              /*!< in: 删除列后表字段的个数 */
    char*                   col_names,          /*!< in: 删除列后用户列所有列名 */
    ulint                   col_names_len,      /*!< in: col_names的长度 */
    uint                    n_cols_before_alter /*!< in: 记录alter前数据字典上n_cols_before_alter的值(包含系统列) */
)
{
    ulint                   org_heap_size = 0;
    dict_col_t*             org_cols;
    /* the fast added columns table's cols num */
    ulint                   org_n_cols = table->n_def - DATA_N_SYS_COLS;
    ulint                   drop_n_cols = org_n_cols - n_col;
    dict_index_t*           index = NULL;
    dict_index_t*           clu_index = NULL;
    dict_field_t*           fields;
    ulint                   i;
    dict_col_t*             drop_col; 
    ulint                   n_drop_col_nullable = 0;

    ut_ad(dict_table_is_gcs(table) && table->cached);
    ut_a(n_col < org_n_cols);
    /* right now the table colums should more than columns altered before */
    ut_ad(table->n_def > n_col + DATA_N_SYS_COLS && table->n_def  == table->n_cols);

    org_heap_size = mem_heap_get_size(table->heap);    

    /* set the n_cols_before_alter_table back to the value before altered */
    table->n_cols_before_alter_table = n_cols_before_alter;


    org_cols = table->cols;

    /* 聚集索引最后几列不处理,但需要把相关标记为置回去 */
    for (i = 0; i < drop_n_cols; ++i)
    {                
        drop_col = org_cols + n_col + i;

        if (!(drop_col->prtype & DATA_NOT_NULL)) {
            n_drop_col_nullable ++;
        }
    }

   
    /* 拷贝前N列（用户列）及列名*/
    table->cols = dict_mem_realloc_dict_cols(table,n_col + DATA_N_SYS_COLS);
    memcpy(table->cols, col_arr, n_col * sizeof(dict_col_t));

    

    /* 还原默认值信息，直接使用原列上的默认值信息，因默认值就是使用table->heap分配的 */
    for (i = 0; i < n_col; ++i)
    {
        if (table->cols[i].def_val)
        {
            //table->cols[i].def_val = col_arr_org[i].def_val;
            table->cols[i].def_val->col = dict_table_get_nth_col(table, i);            
        }
    }

    /* 还原表的列数量信息 */
    table->n_def = n_col;
    table->n_cols = n_col + DATA_N_SYS_COLS;

    table->col_names = dict_mem_realloc_dict_col_names(table,col_names_len);    
    memcpy(table->col_names, col_names, col_names_len);

    /* 增加系统列 */
    table->cached = FALSE;          /* 避免断言 */
    /* 内存分配修改为直接从系统分配,不从heap分配,此处传入NULL值 */
    dict_table_add_system_columns(table, NULL);
    table->cached = TRUE;

    dict_table_set_big_row(table);

    dict_sys->size -= org_heap_size;
    dict_sys->size += mem_heap_get_size(table->heap);

    /* 更新索引及索引列信息 */
    index = UT_LIST_GET_FIRST(table->indexes);
    while (index)
    {
        ulint               n_fields;       
        ibool               is_gen_clust_index = FALSE;

        ut_ad(index->n_def == index->n_fields);

        org_heap_size = mem_heap_get_size(index->heap); 

        if (dict_index_is_clust(index))
        {
            clu_index = index;

            /* 修改主键为rowid的ord_part值 */
            if (!strcmp(clu_index->name, "GEN_CLUST_INDEX"))
            {
                is_gen_clust_index = TRUE;
            }            

            ut_ad(index->n_fields <= index->n_user_defined_cols + table->n_cols + drop_n_cols);

            /* reset the n_nullable */
            index->n_nullable -= n_drop_col_nullable;

            index->n_fields  -= drop_n_cols;
            index->n_def     -= drop_n_cols;

            ut_ad(index->n_fields == index->n_def);
            ut_ad(index->n_fields_before_alter);

            /* 更新index的n_fields_before_alter，只有第一次加字段失败回滚才需要修改该信息 */
            if (table->n_cols_before_alter_table == 0)
            {
                index->n_fields_before_alter = 0;
                index->n_nullable_before_alter = 0;
            }
        }
        else
        {
            /* 非聚集索引,此处内存在加字段时重新分配的,并且不可能有人使用,因此此处内存不重新分配 */
            ut_ad(index->n_user_defined_cols + clu_index->n_uniq + 1 >= index->n_fields);
            ut_ad(clu_index != NULL);            
        }

        n_fields = index->n_fields;
        fields = index->fields;      
        
        /* 修改索引列col和name指针地址 */
        for (i = 0; i < n_fields; ++i)
        {
            ulint       col_ind = fields[i].col_ind;

            /* 因为表中n_col后插入了若干列，原列索引大于等于n_col(系统列)都需要减小增加的列数 */
            if (col_ind >= n_col)
            {
                /*               
                索引字段必定指向系统列
                */
                ut_a(col_ind >= n_col + drop_n_cols);
                col_ind -= drop_n_cols;
               
            }
           
            ut_ad( col_ind < n_col + DATA_N_SYS_COLS);

            fields[i].col = dict_table_get_nth_col(table, col_ind);
            fields[i].col_ind = fields[i].col->ind;

            fields[i].name = dict_table_get_col_name(table, col_ind);

            if (is_gen_clust_index && !strcmp(fields[i].name, "DB_ROW_ID"))
            {
                ut_ad(!fields[i].col->ord_part);
                fields[i].col->ord_part = 1;
            }            
        }

        dict_sys->size -= org_heap_size;
        dict_sys->size += mem_heap_get_size(index->heap);

        index = UT_LIST_GET_NEXT(indexes, index);
    }

    /* 外键 */
}



/************************************************************************/
/* 
根据表的cols的数量,计算存储dict_table->cols需要的分配内存空间的数量(列数量).

dict_table->cols的分配策略是: 
1.新表第一次初始化分配,使用表的默认字段数量做分配.
2.如果进行快速加字段的表操作,则将新表的内存空间设置为:

小于等于10列的表，初始分配10列的内存；
对于大于10列的表，分配ceil(CN/10)*10列的内存空间.
这样不会造成大量不增加字段的表的数据字典的内存浪费.
*/
UNIV_INTERN
void *
dict_mem_realloc_dict_cols(
    dict_table_t * table,  
    ulint          col_num    /*<! in: columns number need to allocate memmory(include the 3 system columns) */
){   
    dict_col_t* new_mem_alloc;
    ulint n_new_cols;

    /* enough memory,return */
    if(table->n_cols_allocated >= col_num){
        return table->cols;
    }    

    
    if(table->n_cols_allocated ==0){
        /* table init,the first time to alloc cols mem */
        table->n_cols_allocated = col_num;
        table->cols = (dict_col_t*)ut_zalloc(sizeof(dict_col_t) * col_num);        
    }else{
        /* realloc */
        n_new_cols = REALLOC_COUNT(DATA_N_INIT_COLS,DATA_N_COLS_INC,col_num);

        ut_a(n_new_cols >= col_num);
        ut_a(col_num <= 1024);

        n_new_cols = n_new_cols > 1024? 1024:n_new_cols;
      
        new_mem_alloc = (dict_col_t*)ut_zalloc(sizeof(dict_col_t)*n_new_cols); 

        memcpy(new_mem_alloc, table->cols, sizeof(dict_col_t)*table->n_cols_allocated);
        ut_free(table->cols);

        table->cols = new_mem_alloc;
        table->n_cols_allocated = n_new_cols;        
    }
    return table->cols;
}



/*****************************************************/
/* 为dict_table 的col_names分配内存空间. 
第一次分配,按照实际数量分配
若涉及到col_names的变动,则重新分配内存空间,并且多分配一部分,当下次再更改时,如果空间足够,则重复使用
否则再分配当前长度加N*DATE_N_COLS_LENGTH_INC的空间
*/
UNIV_INTERN
void *
dict_mem_realloc_dict_col_names(
    dict_table_t *  table,  
    ulint           n_new_cols    /*<! in: new col_names length in Byte */
)
{   
    ulint length_to_alloc;
    char* new_mem_alloc;

    if (table->col_names_length_alloced > n_new_cols)
    {
        return table->col_names;
    } 

    /* init */
    if(table->col_names_length_alloced == 0){

        table->col_names_length_alloced = n_new_cols;
        table->col_names =(char*) ut_zalloc(n_new_cols);   

    }else{

        length_to_alloc = REALLOC_COUNT(DATA_N_INIT_COL_NAMES_LENGTH,DATA_N_COL_NAMES_LENGTH_INC,n_new_cols);

        ut_a(length_to_alloc >= n_new_cols);

        new_mem_alloc = (char*)ut_zalloc(length_to_alloc); 

        memcpy(new_mem_alloc, table->col_names, table->col_names_length_alloced);        
        ut_free(table->col_names); 

        table->col_names = new_mem_alloc;
        table->col_names_length_alloced = length_to_alloc;
    }
    return table->col_names;
}


/****************************************************/
/* allocate memory for index->fields,if need more fields,re-allocate */
UNIV_INTERN
void *
dict_mem_realloc_index_fields(
    dict_index_t *  index,  
    ulint           field_num    /*<! in: index fields number need to allocate memmory(include the 3 system columns) */
)
{
    ulint n_field_to_alloc;
    dict_field_t* new_mem_alloc;

    if(index->n_fields_allocated >= field_num)
    {
        return index->fields;
    }

    if(index->n_fields_allocated == 0){
        index->n_fields_allocated = field_num;
        index->fields = (dict_field_t*)ut_zalloc(1 + field_num * sizeof(dict_field_t));
    }else{
        n_field_to_alloc = REALLOC_COUNT(DATA_N_INDEX_INIT_FIELDS,DATA_N_INDEX_FIELDS_INC,field_num);

        ut_a(n_field_to_alloc >= field_num);
        ut_a(field_num <= 1024);

        n_field_to_alloc = n_field_to_alloc > 1024? 1024 : n_field_to_alloc;

        new_mem_alloc = (dict_field_t *) ut_zalloc(1 + n_field_to_alloc*sizeof(dict_field_t));

        memcpy(new_mem_alloc, index->fields, 1 + (index->n_fields_allocated*sizeof(dict_field_t)));
        ut_free(index->fields);

        index->fields = new_mem_alloc;
        index->n_fields_allocated = n_field_to_alloc;
    }        
    return index->fields;
}



/**********************************************************************//**
快速修改列字符集,修改表对象内存中列的字符集.
直接修改table->cols列对象的字符集规则即可.除了列字符集修改,表对象的其他成员均无变化
*/
void
dict_mem_table_fast_alter_collate(
    dict_table_t*           table,              /*!< in: 原表字典对象 */
    dict_col_t*             col_arr,            /*!< in: 修改列后的用户列字典对象,不包含系统列 */ 
    ulint                   n_modify            /*!< in: col_arr的列数 */
)
{
    ulint i, ind;

    for(i=0; i < n_modify; i++){
        /* 只有ind值合法才修改，不合法列没有修改字符集 */
        ind = col_arr[i].ind;

        ut_ad(ind < table->n_cols);
        table->cols[ind].prtype = col_arr[i].prtype;
    }
}

