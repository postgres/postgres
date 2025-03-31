/*
 * The libkmip specific code need to be in a separate library to avoid
 * collissions with PostgreSQL's header files.
 */

#include <stdio.h>
#include <kmip.h>
#include <kmip_bio.h>
#include <kmip_locate.h>

#include "keyring/keyring_kmip_impl.h"

int
pg_tde_kmip_set_by_name(BIO *bio, char *key_name, const unsigned char *key, unsigned int key_len)
{
	Attribute	a[4];
	enum cryptographic_algorithm algorithm = KMIP_CRYPTOALG_AES;
	int32		length = key_len * 8;
	int32		mask = KMIP_CRYPTOMASK_ENCRYPT | KMIP_CRYPTOMASK_DECRYPT;
	Name		ts;
	TextString	ts2 = {0, 0};
	TemplateAttribute ta = {0};
	char	   *idp = NULL;
	int			id_max_len = 64;

	for (int i = 0; i < 4; i++)
	{
		kmip_init_attribute(&a[i]);
	}

	a[0].type = KMIP_ATTR_CRYPTOGRAPHIC_ALGORITHM;
	a[0].value = &algorithm;

	a[1].type = KMIP_ATTR_CRYPTOGRAPHIC_LENGTH;
	a[1].value = &length;

	a[2].type = KMIP_ATTR_CRYPTOGRAPHIC_USAGE_MASK;
	a[2].value = &mask;

	ts2.value = key_name;
	ts2.size = kmip_strnlen_s(key_name, 250);
	ts.value = &ts2;
	ts.type = KMIP_NAME_UNINTERPRETED_TEXT_STRING;
	a[3].type = KMIP_ATTR_NAME;
	a[3].value = &ts;

	ta.attributes = a;
	ta.attribute_count = ARRAY_LENGTH(a);

	return kmip_bio_register_symmetric_key(bio, &ta, (char *) key, key_len, &idp, &id_max_len);
}

int
pg_tde_kmip_locate_key(BIO *bio, const char *key_name, size_t *ids_found, char *id)
{
	int			upto = 0;
	int			result;
	LocateResponse locate_result;
	Name		ts;
	TextString	ts2 = {0, 0};
	Attribute	a[3];
	enum object_type loctype = KMIP_OBJTYPE_SYMMETRIC_KEY;

	for (int i = 0; i < 3; i++)
	{
		kmip_init_attribute(&a[i]);
	}

	a[0].type = KMIP_ATTR_OBJECT_TYPE;
	a[0].value = &loctype;

	ts2.value = (char *) key_name;
	ts2.size = kmip_strnlen_s(key_name, 250);
	ts.value = &ts2;
	ts.type = KMIP_NAME_UNINTERPRETED_TEXT_STRING;
	a[1].type = KMIP_ATTR_NAME;
	a[1].value = &ts;

	/* 16 is hard coded: seems like the most vault supports? */
	result = kmip_bio_locate(bio, a, 2, &locate_result, 16, upto);

	if (result == 0)
	{
		*ids_found = locate_result.ids_size;

		if (locate_result.ids_size > 0)
			memcpy(id, locate_result.ids[0], MAX_LOCATE_LEN);
	}

	return result;
}

int
pg_tde_kmip_get_key(BIO *bio, char *id, char **key, int *key_len)
{
	return kmip_bio_get_symmetric_key(bio, id, strlen(id), key, key_len);
}
