// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "core/hle/result.h"

namespace Service::SSL {

constexpr Result ResultSslService{ErrorModule::SSLSrv, 0};
constexpr Result ResultUnknown{ErrorModule::SSLSrv, 1};
constexpr Result ResultFatal{ErrorModule::SSLSrv, 9};
constexpr Result ResultFatail{ErrorModule::SSLSrv, 10};
constexpr Result ResultErrorLower{ErrorModule::SSLSrv, 11};
constexpr Result ResultInvalidReference{ErrorModule::SSLSrv, 12};
constexpr Result ResultInternalLogicError{ErrorModule::SSLSrv, 13};

constexpr Result ResultRetryRequired{ErrorModule::SSLSrv, 100};
constexpr Result ResultLibraryNotInitialized{ErrorModule::SSLSrv, 101};
constexpr Result ResultInsufficientMemory{ErrorModule::SSLSrv, 102};
constexpr Result ResultSocketNotRegistered{ErrorModule::SSLSrv, 103};
constexpr Result ResultHostNameNotRegistered{ErrorModule::SSLSrv, 104};
constexpr Result ResultHostNameTooLong{ErrorModule::SSLSrv, 105};
constexpr Result ResultBufferAlreadyRegistered{ErrorModule::SSLSrv, 106};
constexpr Result ResultConnectionRemaining{ErrorModule::SSLSrv, 107};
constexpr Result ResultContextRemaining{ErrorModule::SSLSrv, 108};
constexpr Result ResultInvalidConnectionContext{ErrorModule::SSLSrv, 109};
constexpr Result ResultInvalidContext{ErrorModule::SSLSrv, 110};
constexpr Result ResultInvalidIoMode{ErrorModule::SSLSrv, 111};
constexpr Result ResultBufferTooShort{ErrorModule::SSLSrv, 112};
constexpr Result ResultInvalidPointer{ErrorModule::SSLSrv, 113};
constexpr Result ResultResourceMax{ErrorModule::SSLSrv, 114};
constexpr Result ResultResourceBusy{ErrorModule::SSLSrv, 115};
constexpr Result ResultInvalidSocketDescriptor{ErrorModule::SSLSrv, 116};
constexpr Result ResultInvalidCertFormat{ErrorModule::SSLSrv, 117};
constexpr Result ResultInvalidPasswordSize{ErrorModule::SSLSrv, 118};
constexpr Result ResultNoTcpConnection{ErrorModule::SSLSrv, 119};
constexpr Result ResultInvalidInternalPkiType{ErrorModule::SSLSrv, 120};
constexpr Result ResultInvalidSessionCacheMode{ErrorModule::SSLSrv, 121};
constexpr Result ResultInvalidRenegotiationMode{ErrorModule::SSLSrv, 122};
constexpr Result ResultInvalidConcurrencyLimit{ErrorModule::SSLSrv, 123};
constexpr Result ResultPolicyOidStringTooLong{ErrorModule::SSLSrv, 124};
constexpr Result ResultInvalidPolicyOidStringBufferLength{ErrorModule::SSLSrv, 125};
constexpr Result ResultInvalidOptionType{ErrorModule::SSLSrv, 126};
constexpr Result ResultInvalidIdCount{ErrorModule::SSLSrv, 127};
constexpr Result ResultInvalidIndex{ErrorModule::SSLSrv, 128};
constexpr Result ResultInvalidCrlFormat{ErrorModule::SSLSrv, 129};
constexpr Result ResultUnknownCrlIssuer{ErrorModule::SSLSrv, 130};
constexpr Result ResultCrlSignatureVerifyFail{ErrorModule::SSLSrv, 131};
constexpr Result ResultInvalidArgument{ErrorModule::SSLSrv, 132};
constexpr Result ResultOperationNotAllowed{ErrorModule::SSLSrv, 133};
constexpr Result ResultCommonNameTooLong{ErrorModule::SSLSrv, 134};

constexpr Result ResultReport{ErrorModule::SSLSrv, 200};
constexpr Result ResultLibraryAlreadyInitialized{ErrorModule::SSLSrv, 201};
constexpr Result ResultInsufficientServerCertBuffer{ErrorModule::SSLSrv, 202};
constexpr Result ResultSocketAlreadyRegistered{ErrorModule::SSLSrv, 203};
constexpr Result ResultIoWouldBlock{ErrorModule::SSLSrv, 204};
constexpr Result ResultIoTimeout{ErrorModule::SSLSrv, 205};
constexpr Result ResultIoAborted{ErrorModule::SSLSrv, 206};
constexpr Result ResultVerifyCertFailed{ErrorModule::SSLSrv, 207};
constexpr Result ResultNoConnection{ErrorModule::SSLSrv, 208};
constexpr Result ResultConnectionReset{ErrorModule::SSLSrv, 209};
constexpr Result ResultConnectionAborted{ErrorModule::SSLSrv, 210};
constexpr Result ResultSocketShutdown{ErrorModule::SSLSrv, 211};
constexpr Result ResultNetworkDown{ErrorModule::SSLSrv, 212};
constexpr Result ResultInvalidCertificate{ErrorModule::SSLSrv, 213};
constexpr Result ResultInvalidCertStoreId{ErrorModule::SSLSrv, 214};
constexpr Result ResultClientPkiAlreadyRegistered{ErrorModule::SSLSrv, 215};
constexpr Result ResultNoSslConnection{ErrorModule::SSLSrv, 216};
constexpr Result ResultConnectionClosed{ErrorModule::SSLSrv, 217};
constexpr Result ResultMaxServerPkiRegistered{ErrorModule::SSLSrv, 218};
constexpr Result ResultMaxPolicyOidRegistered{ErrorModule::SSLSrv, 219};
constexpr Result ResultSslCoreInitializing{ErrorModule::SSLSrv, 220};
constexpr Result ResultNoServerChain{ErrorModule::SSLSrv, 221};
constexpr Result ResultClientPkiNotRegistered{ErrorModule::SSLSrv, 222};
constexpr Result ResultNotInDtlsMode{ErrorModule::SSLSrv, 223};
constexpr Result ResultNotPerformingHandshake{ErrorModule::SSLSrv, 224};
constexpr Result ResultAllocatorAlreadySet{ErrorModule::SSLSrv, 225};

constexpr Result ResultSslError{ErrorModule::SSLSrv, 300};
constexpr Result ResultSslErrorInvalidCertDomain{ErrorModule::SSLSrv, 301};
constexpr Result ResultSslErrorNoCertificate{ErrorModule::SSLSrv, 302};
constexpr Result ResultSslErrorExpiredCertificate{ErrorModule::SSLSrv, 303};
constexpr Result ResultSslErrorRevokedCertificate{ErrorModule::SSLSrv, 304};
constexpr Result ResultSslErrorUnsupportedCertificate{ErrorModule::SSLSrv, 305};
constexpr Result ResultSslErrorUnknownCertificate{ErrorModule::SSLSrv, 306};
constexpr Result ResultSslErrorBadCertificate{ErrorModule::SSLSrv, 307};
constexpr Result ResultSslErrorUnkownCa{ErrorModule::SSLSrv, 308};
constexpr Result ResultSslErrorAccessDenied{ErrorModule::SSLSrv, 309};
constexpr Result ResultSslErrorReceivedUnkownData{ErrorModule::SSLSrv, 310};
constexpr Result ResultSslErrorReceivedMalformedData{ErrorModule::SSLSrv, 311};
constexpr Result ResultSslErrorReceivedUnexpectedHandshakeData{ErrorModule::SSLSrv, 312};
constexpr Result ResultSslErrorReceivedUnexpectedRecordData{ErrorModule::SSLSrv, 313};
constexpr Result ResultSslErrorReceivedTooLongRecord{ErrorModule::SSLSrv, 314};
constexpr Result ResultSslErrorUnexpectedHelloVerifyRequest{ErrorModule::SSLSrv, 315};
constexpr Result ResultSslErrorUnexpectedCertificateStatusResponse{ErrorModule::SSLSrv, 316};
constexpr Result ResultSslErrorInconsistentSignAlgorithm{ErrorModule::SSLSrv, 317};
constexpr Result ResultSslErrorInsufficientSecurity{ErrorModule::SSLSrv, 318};
constexpr Result ResultSslErrorDecryptionWithInvalidMac{ErrorModule::SSLSrv, 319};
constexpr Result ResultSslErrorDigestCalculationFailed{ErrorModule::SSLSrv, 321};
constexpr Result ResultSslErrorMacCalculationFailed{ErrorModule::SSLSrv, 322};
constexpr Result ResultSslErrorClientCertificateNotFound{ErrorModule::SSLSrv, 323};
constexpr Result ResultSslErrorNoExtendedField{ErrorModule::SSLSrv, 324};
constexpr Result ResultSslErrorUntrustedOid{ErrorModule::SSLSrv, 325};
constexpr Result ResultSslErrorInvalidPolicy{ErrorModule::SSLSrv, 326};
constexpr Result ResultSslErrorNoCipher{ErrorModule::SSLSrv, 327};
constexpr Result ResultSslErrorUnknownEvFingerprint{ErrorModule::SSLSrv, 328};
constexpr Result ResultSslErrorDecompressionFailed{ErrorModule::SSLSrv, 330};

constexpr Result ResultSslAlert{ErrorModule::SSLSrv, 1500};
constexpr Result ResultSslAlertCloseNotify{ErrorModule::SSLSrv, 1501};
constexpr Result ResultSslAlertUnexpectedMessage{ErrorModule::SSLSrv, 1502};
constexpr Result ResultSslAlertBadRecordMac{ErrorModule::SSLSrv, 1503};
constexpr Result ResultSslAlertDecriptionFailed{ErrorModule::SSLSrv, 1504};
constexpr Result ResultSslAlertRecordOverfrlow{ErrorModule::SSLSrv, 1505};
constexpr Result ResultSslAlertDecompressionFailure{ErrorModule::SSLSrv, 1506};
constexpr Result ResultSslAlertHandshakeFailure{ErrorModule::SSLSrv, 1507};
constexpr Result ResultSslAlertNoCertificate{ErrorModule::SSLSrv, 1508};
constexpr Result ResultSslAlertBadCertificate{ErrorModule::SSLSrv, 1509};
constexpr Result ResultSslAlertUnsupportedCertificate{ErrorModule::SSLSrv, 1510};
constexpr Result ResultSslAlertCertificateRevoked{ErrorModule::SSLSrv, 1511};
constexpr Result ResultSslAlertCertificateExpired{ErrorModule::SSLSrv, 1512};
constexpr Result ResultSslAlertCertificateUnkown{ErrorModule::SSLSrv, 1513};
constexpr Result ResultSslAlertIllegalParameter{ErrorModule::SSLSrv, 1514};
constexpr Result ResultSslAlertUnknownCa{ErrorModule::SSLSrv, 1515};
constexpr Result ResultSslAlertAccessDenied{ErrorModule::SSLSrv, 1516};
constexpr Result ResultSslAlertDecodeError{ErrorModule::SSLSrv, 1517};
constexpr Result ResultSslAlertDecryptError{ErrorModule::SSLSrv, 1518};
constexpr Result ResultSslAlertExportRestriction{ErrorModule::SSLSrv, 1519};
constexpr Result ResultSslAlertProtocolVersion{ErrorModule::SSLSrv, 1520};
constexpr Result ResultSslAlertInsufficientSecurity{ErrorModule::SSLSrv, 1521};
constexpr Result ResultSslAlertInternalError{ErrorModule::SSLSrv, 1522};
constexpr Result ResultSslAlertInappropriateFallback{ErrorModule::SSLSrv, 1523};
constexpr Result ResultSslAlertUserCancelled{ErrorModule::SSLSrv, 1524};
constexpr Result ResultSslAlertNoRenegotiation{ErrorModule::SSLSrv, 1525};
constexpr Result ResultSslAlertUnsupportedExtention{ErrorModule::SSLSrv, 1526};
constexpr Result ResultSslAlertCertificateUnobtainable{ErrorModule::SSLSrv, 1527};
constexpr Result ResultSslAlertUnrecognizedName{ErrorModule::SSLSrv, 1528};
constexpr Result ResultSslAlertBadCertificateStatusResponse{ErrorModule::SSLSrv, 1529};
constexpr Result ResultSslAlertBadCertificateHashValue{ErrorModule::SSLSrv, 1530};
constexpr Result ResultSslAlertUnkownPskIdentity{ErrorModule::SSLSrv, 1531};
constexpr Result ResultSslAlertNoApplicationProtocol{ErrorModule::SSLSrv, 1532};

} // namespace Service::SSL
