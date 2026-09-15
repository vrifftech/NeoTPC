#pragma once

#include <neoshared/texture/Txi.hpp>

namespace neotpc::texture {
using neoshared::texture::TxiDirectiveValueKind;
using neoshared::texture::TxiIssueSeverity;
using neoshared::texture::TxiDirectiveInfo;
using neoshared::texture::TxiEntry;
using neoshared::texture::TxiValidationIssue;
using neoshared::texture::TxiAutocompleteKind;
using neoshared::texture::TxiAutocompleteResult;
using neoshared::texture::txiDirectiveValueKindToString;
using neoshared::texture::txiIssueSeverityToString;
using neoshared::texture::txiDirectiveCatalog;
using neoshared::texture::findTxiDirective;
using neoshared::texture::parseTxiEntries;
using neoshared::texture::validateTxiText;
using neoshared::texture::txiValidationReport;
using neoshared::texture::txiCatalogReport;
using neoshared::texture::txiKeyReferenceText;
using neoshared::texture::txiAutocomplete;
using neoshared::texture::txiDirectiveSignature;
using neoshared::texture::txiDirectiveHint;
using neoshared::texture::txiValueHint;

} // namespace neotpc::texture
