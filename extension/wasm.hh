/*
  +----------------------------------------------------------------------+
  | PHP Version 7                                                        |
  +----------------------------------------------------------------------+
  | Copyright (c) 1997-2019 The PHP Group                                |
  +----------------------------------------------------------------------+
  | This source file is subject to version 3.01 of the PHP license,      |
  | that is bundled with this package in the file LICENSE, and is        |
  | available through the world-wide-web at the following url:           |
  | http://www.php.net/license/3_01.txt                                  |
  | If you did not receive a copy of the PHP license and are unable to   |
  | obtain it through the world-wide-web, please send a note to          |
  | license@php.net so we can mail you a copy immediately.               |
  +----------------------------------------------------------------------+
  | Author: Ivan Enderlin                                                |
  +----------------------------------------------------------------------+
*/

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include "php.h"
#include "ext/standard/info.h"
#include "zend_exceptions.h"
#include "Zend/zend_interfaces.h"
#include "php_wasm.h"
#include "wasmer.hh"

#if defined(PHP_WIN32)
#  include "win32/php_stdint.h"
#elif defined(HAVE_STDINT_H)
#  include <stdint.h>
#endif

// Constant to represent a not nullable (return) type.
#define NOT_NULLABLE 0

// Constant to represent a nullable (return) type.
#define NULLABLE 1

// Syntactic sugar to represent the arity of a function in
// `ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX`.
#define ARITY(n) n

/**
 * Class entry for the `WasmArrayBuffer` class.
 */
zend_class_entry *wasm_array_buffer_class_entry;
zend_object_handlers wasm_array_buffer_class_entry_handlers;

/**
 * Custom object for the `WasmArrayBuffer` class.
 */
typedef struct {
    // The internal buffer.
    int8_t *buffer;

    // The internal buffer length.
    size_t buffer_length;

    // A flag to indicate whether the buffer has been allocated or not.
    bool allocated_buffer;

    // The class instance, i.e. the object. It must be the last item
    // of the structure.
    zend_object instance;
} wasm_array_buffer_object;

/**
 * Gets the `wasm_array_buffer_object` pointer from a `zend_object` pointer.
 */
static inline wasm_array_buffer_object *wasm_array_buffer_object_from_zend_object(zend_object *object);

/**
 * Function for a `zend_class_entry` to create a `WasmArrayBuffer` object.
 */
static zend_object *create_wasm_array_buffer_object(zend_class_entry *class_entry);

/**
 * Handler for a `zend_class_entry` to destroy (i.e. call the
 * destructor on the userland) a `WasmArrayBuffer` object.
 */
static void destroy_wasm_array_buffer_object(zend_object *object);

/**
 * Handler for a `zend_class_entry` to free a `WasmArrayBuffer` object.
 */
static void free_wasm_array_buffer_object(zend_object *object);

// Shortcut to get `$this` in a `WasmArrayBuffer` method.
#define WASM_ARRAY_BUFFER_OBJECT_THIS() wasm_array_buffer_object_from_zend_object(Z_OBJ_P(getThis()))

/**
 * Utils.
 */
wasmer_value_tag from_zend_long_to_wasmer_value_tag(zend_long x);

/**
 * Information for the `wasm_bytes` resource.
 */
const char* wasm_bytes_resource_name;
int wasm_bytes_resource_number;

/**
 * Extract the data structure inside the `wasm_bytes` resource.
 */
wasmer_byte_array *wasm_bytes_from_resource(zend_resource *wasm_bytes_resource);

/**
 * Destructor for the `wasm_bytes` resource.
 */
static void wasm_bytes_destructor(zend_resource *resource);

/**
 * Information for the `wasm_module` resource.
 */
const char* wasm_module_resource_name;
int wasm_module_resource_number;

/**
 * Extract the data structure inside the `wasm_module` resource.
 */
wasmer_module_t *wasm_module_from_resource(zend_resource *wasm_module_resource);

/**
 * Destructor for the `wasm_module` resource.
 */
static void wasm_module_destructor(zend_resource *resource);


/**
 * Clean up all persistent resources registered by this module.
 */
static int clean_up_persistent_resources(zval *hashmap_item);

/**
 * Iterate over the persistent resources list to clean up Wasm
 * persistent resources.
 */
static void php_wasm_module_clean_up_persistent_resources();

/**
 * Information for the `wasm_instance` resource.
 */
const char* wasm_instance_resource_name;
int wasm_instance_resource_number;

/**
 * Extract the data structure inside the `wasm_instance` resource.
 */
wasmer_instance_t *wasm_instance_from_resource(zend_resource *wasm_instance_resource);

/**
 * Destructor for the `wasm_instance` resource.
 */
static void wasm_instance_destructor(zend_resource *resource);

/**
 * Information for the `wasm_value` resource.
 */
const char* wasm_value_resource_name;
int wasm_value_resource_number;

/**
 * Extract the data structure inside the `wasm_value` resource.
 */
wasmer_value_t *wasm_value_from_resource(zend_resource *wasm_value_resource);

/**
 * Destructor for the `wasm_value` resource.
 */
static void wasm_value_destructor(zend_resource *resource);

/**
 * Class entries for the `WasmTypeArray` classes. The all share the
 * same implementation, i.e. they use the same class entry handlers.
 */
zend_class_entry *wasm_typed_array_int8_class_entry;
zend_class_entry *wasm_typed_array_uint8_class_entry;
zend_class_entry *wasm_typed_array_int16_class_entry;
zend_class_entry *wasm_typed_array_uint16_class_entry;
zend_class_entry *wasm_typed_array_int32_class_entry;
zend_class_entry *wasm_typed_array_uint32_class_entry;
zend_object_handlers wasm_typed_array_class_entry_handlers;

/**
 * All types of `WasmTypedArray` views.
 */
typedef enum {
    INT8,
    UINT8,
    INT16,
    UINT16,
    INT32,
    UINT32
} wasm_typed_array_kind;

/**
 * Custom object for the `WasmTypedArray` classes. `WasmTypedArray` is
 * a generic name used here to represent all classes like
 * `WasmInt8Array`, `WasmUint8Array` etc. All these classes share the
 * same implementation.
 */
typedef struct {
    // The type of the typed array. Set by the `create_object` class
    // entry item.
    wasm_typed_array_kind kind;

    // The internal `WasmArrayBuffer`. Set by the `__construct`
    // method.
    zval *wasm_array_buffer;

    // The offset over the buffer, i.e. start reading the internal
    // buffer at this offset. Set by the `__construct` method.
    size_t offset;

    // The length of the view, i.e. read the internal buffer from the
    // offset to this length. Set by the `__construct` method.
    size_t length;

    // The buffer view over this `wasm_array_buffer`. Set by the
    // `__construct` method.
    union {
        int8_t *as_int8;
        uint8_t *as_uint8;
        int16_t *as_int16;
        uint16_t *as_uint16;
        int32_t *as_int32;
        uint32_t *as_uint32;
    } view;

    // The class instance, i.e. the object. It must be the last item
    // of the structure.
    zend_object instance;
} wasm_typed_array_object;

/**
 * Gets the `wasm_typed_array_object` pointer from a `zend_object` pointer.
 */
static inline wasm_typed_array_object *wasm_typed_array_object_from_zend_object(zend_object *object);

/**
 * Function for a `zend_class_entry` to create one of the `WasmTypedArray` objects.
 */
static zend_object *create_wasm_typed_array_object(zend_class_entry *class_entry);

/**
 * Handler for a `zend_class_entry` to destroy (i.e. call the
 * destructor on the userland) of one of the `WasmTypedArray` objects.
 */
static void destroy_wasm_typed_array_object(zend_object *object);

/**
 * Handler for a `zend_class_entry` to free one of the `WasmTypedArray` objects.
 */
static void free_wasm_typed_array_object(zend_object *object);

// Shortcut to get `$this` in a `WasmTypedArray` method.
#define WASM_TYPED_ARRAY_OBJECT_THIS() wasm_typed_array_object_from_zend_object(Z_OBJ_P(getThis()))

/*
 * Local variables:
 * tab-width: 4
 * c-basic-offset: 4
 * End:
 */
