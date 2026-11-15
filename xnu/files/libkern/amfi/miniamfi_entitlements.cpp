/*
 * Entitlement storage and queries for the AMFI stand-in (see miniamfi.c).
 *
 * Each validated cs_blob holds an OSArray in csb_entitlements:
 *   [0] OSDictionary: parsed XML entitlements for in-kernel and IOKit queries
 *   [1] OSData:       verbatim CS_GenericBlob (0xfade7171) for CS_OPS_ENTITLEMENTS_BLOB
 *   [2] OSData:       transmuted DER v1 CS_GenericBlob (0xfade7172) when the
 *                     signature lacks an embedded CSSLOT_DER_ENTITLEMENTS slot
 *
 * Storing an OSArray lets csblob_os_entitlements_set() and cs_blob_cleanup()
 * manage object lifetimes directly through osobject_retain/osobject_release.
 */

#include <libkern/c++/OSArray.h>
#include <libkern/c++/OSBoolean.h>
#include <libkern/c++/OSCollectionIterator.h>
#include <libkern/c++/OSData.h>
#include <libkern/c++/OSDictionary.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSString.h>
#include <libkern/c++/OSSymbol.h>
#include <libkern/c++/OSUnserialize.h>
#include <libkern/OSByteOrder.h>
#include <kern/cs_blobs.h>
#include <mach/kern_return.h>
#include <kern/kalloc.h>
#include <string.h>

extern "C" {
struct proc;
struct cs_blob;

struct cs_blob *csproc_get_blob(struct proc *p);
int cs_entitlements_blob_get(struct proc *p, void **out_start, size_t *out_length);
int csblob_get_entitlements(struct cs_blob *csblob, void **out_start, size_t *out_length);
int csblob_get_der_entitlements(struct cs_blob *csblob, const CS_GenericBlob **out_start, size_t *out_length);
void csblob_os_entitlements_set(struct cs_blob *csblob, void *entitlements);
void *csblob_os_entitlements_get(struct cs_blob *csblob);

void miniamfi_blob_entitlements_init(struct cs_blob *cs_blob);
void *miniamfi_osent_asdict(void *osentitlements);
bool miniamfi_osent_get_transmuted(void *osentitlements, const CS_GenericBlob **blob);
bool miniamfi_osent_get_xml(void *osentitlements, CS_GenericBlob **blob);
kern_return_t miniamfi_osent_boolean(const void *os_entitlements, const char *name);
kern_return_t miniamfi_osent_string(const void *os_entitlements, const char *name, const char *value);
kern_return_t miniamfi_osent_object(const void *os_entitlements, const char *name, void **object);

kern_return_t miniamfi_entitlement_boolean(struct proc *proc, const char *name);
kern_return_t miniamfi_entitlement_string(struct proc *proc, const char *name, const char *value);
kern_return_t miniamfi_entitlement_object(struct proc *proc, const char *name, void **object);
}

static OSDictionary *
parse_xml_entitlements(const void *start, size_t length)
{
	size_t text_length;
	OSObject *parsed;
	OSDictionary *dict;
	char *text;

	if (start == NULL || length <= sizeof(CS_GenericBlob)) {
		return NULL;
	}
	/*
	 * The blob is a header followed by the text of an XML property list.
	 * OSUnserializeXML expects a NUL-terminated buffer.
	 */
	text_length = length - sizeof(CS_GenericBlob);
	text = (char *)kalloc_data(text_length + 1, Z_WAITOK);
	if (text == NULL) {
		return NULL;
	}
	memcpy(text, ((const CS_GenericBlob *)start)->data, text_length);
	text[text_length] = '\0';
	parsed = OSUnserializeXML(text, NULL);
	kfree_data(text, text_length + 1);

	dict = OSDynamicCast(OSDictionary, parsed);
	if (dict == NULL && parsed != NULL) {
		parsed->release();
	}
	return dict;
}

static bool
append_der_tlv(OSData *out, uint8_t tag, const void *body, unsigned int len)
{
	uint8_t hdr[6];
	unsigned int hlen = 0;

	hdr[hlen++] = tag;
	if (len < 0x80u) {
		hdr[hlen++] = (uint8_t)len;
	} else if (len <= 0xffu) {
		hdr[hlen++] = 0x81u;
		hdr[hlen++] = (uint8_t)len;
	} else if (len <= 0xffffu) {
		hdr[hlen++] = 0x82u;
		hdr[hlen++] = (uint8_t)(len >> 8);
		hdr[hlen++] = (uint8_t)len;
	} else if (len <= 0xffffffu) {
		hdr[hlen++] = 0x83u;
		hdr[hlen++] = (uint8_t)(len >> 16);
		hdr[hlen++] = (uint8_t)(len >> 8);
		hdr[hlen++] = (uint8_t)len;
	} else {
		hdr[hlen++] = 0x84u;
		hdr[hlen++] = (uint8_t)(len >> 24);
		hdr[hlen++] = (uint8_t)(len >> 16);
		hdr[hlen++] = (uint8_t)(len >> 8);
		hdr[hlen++] = (uint8_t)len;
	}
	if (!out->appendBytes(hdr, hlen)) {
		return false;
	}
	return len == 0 || out->appendBytes(body, len);
}

static bool
encode_der_value(const OSObject *obj, OSData *out, unsigned int depth)
{
	if (obj == NULL || depth > 8) {
		return false;
	}
	if (const OSBoolean *b = OSDynamicCast(OSBoolean, obj)) {
		uint8_t v = b->isTrue() ? 0xffu : 0x00u;
		return append_der_tlv(out, 0x01u, &v, 1);
	}
	if (const OSString *s = OSDynamicCast(OSString, obj)) {
		const char *cstr = s->getCStringNoCopy();
		unsigned int slen = cstr != NULL ? (unsigned int)strlen(cstr) : 0;
		return append_der_tlv(out, 0x0cu, cstr, slen);
	}
	if (const OSData *d = OSDynamicCast(OSData, obj)) {
		return append_der_tlv(out, 0x04u, d->getBytesNoCopy(), d->getLength());
	}
	if (const OSNumber *n = OSDynamicCast(OSNumber, obj)) {
		uint64_t u = n->unsigned64BitValue();
		uint8_t buf[8];
		for (int i = 0; i < 8; i++) {
			buf[i] = (uint8_t)(u >> ((7 - i) * 8));
		}
		unsigned int start = 0;
		while (start < 7 && buf[start] == 0x00u && (buf[start + 1] & 0x80u) == 0) {
			start++;
		}
		while (start < 7 && buf[start] == 0xffu && (buf[start + 1] & 0x80u) != 0) {
			start++;
		}
		return append_der_tlv(out, 0x02u, &buf[start], 8u - start);
	}
	if (const OSArray *arr = OSDynamicCast(OSArray, obj)) {
		OSData *body = OSData::withCapacity(64);
		if (body == NULL) {
			return false;
		}
		bool ok = true;
		for (unsigned int i = 0; ok && i < arr->getCount(); i++) {
			ok = encode_der_value(arr->getObject(i), body, depth + 1);
		}
		if (ok) {
			ok = append_der_tlv(out, 0x30u, body->getBytesNoCopy(), body->getLength());
		}
		body->release();
		return ok;
	}
	if (const OSDictionary *dict = OSDynamicCast(OSDictionary, obj)) {
		unsigned int count = dict->getCount();
		OSData *body = OSData::withCapacity(count * 32 + 8);
		if (body == NULL) {
			return false;
		}
		const OSSymbol **keys = NULL;
		unsigned int nkeys = 0;
		if (count > 0) {
			keys = (const OSSymbol **)kalloc_data(count * sizeof(*keys), Z_WAITOK);
			if (keys == NULL) {
				body->release();
				return false;
			}
			OSCollectionIterator *it = OSCollectionIterator::withCollection(dict);
			if (it != NULL) {
				while (nkeys < count) {
					const OSSymbol *sym = OSDynamicCast(OSSymbol, it->getNextObject());
					if (sym == NULL) {
						break;
					}
					keys[nkeys++] = sym;
				}
				it->release();
			}
			/* CoreEntitlements DER v1 orders dictionary entries by UTF-8 key. */
			for (unsigned int i = 1; i < nkeys; i++) {
				const OSSymbol *cur = keys[i];
				const char *ck = cur->getCStringNoCopy();
				unsigned int j = i;
				while (j > 0 && strcmp(keys[j - 1]->getCStringNoCopy(), ck) > 0) {
					keys[j] = keys[j - 1];
					j--;
				}
				keys[j] = cur;
			}
		}
		bool ok = true;
		for (unsigned int i = 0; ok && i < nkeys; i++) {
			const char *kstr = keys[i]->getCStringNoCopy();
			OSObject *val = dict->getObject(keys[i]);
			OSData *pair = OSData::withCapacity(64);
			if (pair == NULL) {
				ok = false;
				break;
			}
			ok = append_der_tlv(pair, 0x0cu, kstr, (unsigned int)strlen(kstr)) &&
			    encode_der_value(val, pair, depth + 1) &&
			    append_der_tlv(body, 0x30u, pair->getBytesNoCopy(), pair->getLength());
			pair->release();
		}
		if (keys != NULL) {
			kfree_data(keys, count * sizeof(*keys));
		}
		if (ok) {
			ok = append_der_tlv(out, 0xb0u, body->getBytesNoCopy(), body->getLength());
		}
		body->release();
		return ok;
	}
	return false;
}

static OSData *
encode_der_entitlements_blob(const OSDictionary *dict)
{
	static const uint8_t ver1[] = { 0x02u, 0x01u, 0x01u };
	OSData *app_body = OSData::withCapacity(128);
	if (app_body == NULL) {
		return NULL;
	}
	if (!app_body->appendBytes(ver1, sizeof(ver1)) || !encode_der_value(dict, app_body, 0)) {
		app_body->release();
		return NULL;
	}
	OSData *der = OSData::withCapacity(app_body->getLength() + 16);
	if (der == NULL) {
		app_body->release();
		return NULL;
	}
	uint8_t zero_hdr[sizeof(CS_GenericBlob)] = { 0 };
	bool ok = der->appendBytes(zero_hdr, sizeof(zero_hdr)) &&
	    append_der_tlv(der, 0x70u, app_body->getBytesNoCopy(), app_body->getLength());
	app_body->release();
	if (!ok) {
		der->release();
		return NULL;
	}
	CS_GenericBlob *hdr = (CS_GenericBlob *)const_cast<void *>(der->getBytesNoCopy());
	hdr->magic = OSSwapHostToBigInt32(CSMAGIC_EMBEDDED_DER_ENTITLEMENTS);
	hdr->length = OSSwapHostToBigInt32(der->getLength());
	return der;
}

static OSDictionary *
holder_dict(const void *osentitlements)
{
	const OSArray *holder = OSDynamicCast(OSArray, (const OSObject *)osentitlements);
	if (holder == NULL) {
		return NULL;
	}
	return OSDynamicCast(OSDictionary, holder->getObject(0));
}

static OSData *
holder_blob(const void *osentitlements, unsigned int index)
{
	const OSArray *holder = OSDynamicCast(OSArray, (const OSObject *)osentitlements);
	if (holder == NULL) {
		return NULL;
	}
	return OSDynamicCast(OSData, holder->getObject(index));
}

void
miniamfi_blob_entitlements_init(struct cs_blob *cs_blob)
{
	void *xml_start = NULL;
	size_t xml_len = 0;
	const CS_GenericBlob *der_start = NULL;
	size_t der_len = 0;

	if (cs_blob == NULL || csblob_os_entitlements_get(cs_blob) != NULL) {
		return;
	}
	if (csblob_get_entitlements(cs_blob, &xml_start, &xml_len) != 0 ||
	    xml_start == NULL || xml_len <= sizeof(CS_GenericBlob)) {
		return;
	}
	OSDictionary *dict = parse_xml_entitlements(xml_start, xml_len);
	if (dict == NULL) {
		return;
	}
	OSData *xml_data = OSData::withBytes(xml_start, (unsigned int)xml_len);
	if (xml_data == NULL) {
		dict->release();
		return;
	}
	OSData *der_data = NULL;
	if (csblob_get_der_entitlements(cs_blob, &der_start, &der_len) != 0 ||
	    der_start == NULL || der_len == 0) {
		der_data = encode_der_entitlements_blob(dict);
	}
	OSArray *holder = OSArray::withCapacity(der_data != NULL ? 3 : 2);
	if (holder != NULL && holder->setObject(0, dict) && holder->setObject(1, xml_data)) {
		if (der_data != NULL) {
			holder->setObject(2, der_data);
		}
		csblob_os_entitlements_set(cs_blob, holder);
	}
	if (holder != NULL) {
		holder->release();
	}
	if (der_data != NULL) {
		der_data->release();
	}
	xml_data->release();
	dict->release();
}

void *
miniamfi_osent_asdict(void *osentitlements)
{
	OSDictionary *dict = holder_dict(osentitlements);
	if (dict != NULL) {
		dict->retain();
	}
	return dict;
}

bool
miniamfi_osent_get_transmuted(void *osentitlements, const CS_GenericBlob **blob)
{
	OSData *der = holder_blob(osentitlements, 2);
	if (der == NULL || der->getLength() < sizeof(CS_GenericBlob)) {
		*blob = NULL;
		return false;
	}
	*blob = (const CS_GenericBlob *)der->getBytesNoCopy();
	return true;
}

bool
miniamfi_osent_get_xml(void *osentitlements, CS_GenericBlob **blob)
{
	OSData *xml = holder_blob(osentitlements, 1);
	*blob = NULL;
	if (xml == NULL || xml->getLength() < sizeof(CS_GenericBlob)) {
		return false;
	}
	const CS_GenericBlob *src = (const CS_GenericBlob *)xml->getBytesNoCopy();
	size_t len = (size_t)OSSwapBigToHostInt32(src->length);
	if (len < sizeof(CS_GenericBlob) || len > xml->getLength()) {
		return false;
	}
	/* kern_proc.c CS_OPS_ENTITLEMENTS_BLOB frees *blob with kfree_data(start, length). */
	CS_GenericBlob *copy = (CS_GenericBlob *)kalloc_data(len, Z_WAITOK);
	if (copy == NULL) {
		return false;
	}
	memcpy(copy, src, len);
	*blob = copy;
	return true;
}

static kern_return_t
dict_query_boolean(const OSDictionary *dict, const char *name)
{
	if (dict == NULL || name == NULL) {
		return KERN_NOT_FOUND;
	}
	return dict->getObject(name) == kOSBooleanTrue ? KERN_SUCCESS : KERN_NOT_FOUND;
}

static kern_return_t
dict_query_string(const OSDictionary *dict, const char *name, const char *value)
{
	if (dict == NULL || name == NULL || value == NULL) {
		return KERN_NOT_FOUND;
	}
	OSObject *object = dict->getObject(name);
	OSString *string = OSDynamicCast(OSString, object);
	OSArray *array = OSDynamicCast(OSArray, object);

	if (string != NULL && string->isEqualTo(value)) {
		return KERN_SUCCESS;
	}
	for (unsigned int i = 0; array != NULL && i < array->getCount(); i++) {
		string = OSDynamicCast(OSString, array->getObject(i));
		if (string != NULL && string->isEqualTo(value)) {
			return KERN_SUCCESS;
		}
	}
	return KERN_NOT_FOUND;
}

static kern_return_t
dict_copy_object(const OSDictionary *dict, const char *name, void **object)
{
	OSObject *value = NULL;

	if (dict != NULL && name != NULL) {
		value = dict->getObject(name);
		if (value != NULL) {
			value->retain();
		}
	}
	*object = value;
	return value != NULL ? KERN_SUCCESS : KERN_NOT_FOUND;
}

kern_return_t
miniamfi_osent_boolean(const void *os_entitlements, const char *name)
{
	return dict_query_boolean(holder_dict(os_entitlements), name);
}

kern_return_t
miniamfi_osent_string(const void *os_entitlements, const char *name, const char *value)
{
	return dict_query_string(holder_dict(os_entitlements), name, value);
}

kern_return_t
miniamfi_osent_object(const void *os_entitlements, const char *name, void **object)
{
	return dict_copy_object(holder_dict(os_entitlements), name, object);
}

/* The entitlements of a process as a dictionary, or NULL; the caller releases it. */
static OSDictionary *
copy_entitlements(struct proc *proc)
{
	void *start = NULL;
	size_t length = 0;

	if (proc == NULL) {
		return NULL;
	}
	struct cs_blob *blob = csproc_get_blob(proc);
	if (blob != NULL) {
		OSDictionary *cached = holder_dict(csblob_os_entitlements_get(blob));
		if (cached != NULL) {
			cached->retain();
			return cached;
		}
	}
	if (cs_entitlements_blob_get(proc, &start, &length) != 0) {
		return NULL;
	}
	return parse_xml_entitlements(start, length);
}

kern_return_t
miniamfi_entitlement_boolean(struct proc *proc, const char *name)
{
	OSDictionary *entitlements = copy_entitlements(proc);
	kern_return_t result = dict_query_boolean(entitlements, name);
	if (entitlements != NULL) {
		entitlements->release();
	}
	return result;
}

kern_return_t
miniamfi_entitlement_string(struct proc *proc, const char *name, const char *value)
{
	OSDictionary *entitlements = copy_entitlements(proc);
	kern_return_t result = dict_query_string(entitlements, name, value);
	if (entitlements != NULL) {
		entitlements->release();
	}
	return result;
}

/* The value itself, retained, as IOKit wants it. */
kern_return_t
miniamfi_entitlement_object(struct proc *proc, const char *name, void **object)
{
	OSDictionary *entitlements = copy_entitlements(proc);
	kern_return_t result = dict_copy_object(entitlements, name, object);
	if (entitlements != NULL) {
		entitlements->release();
	}
	return result;
}
