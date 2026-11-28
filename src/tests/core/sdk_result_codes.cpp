// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <catch2/catch_test_macros.hpp>

#include "core/file_sys/errors.h"
#include "core/hle/result.h"
#include "core/hle/service/acc/errors.h"
#include "core/hle/service/am/oe_results.h"
#include "core/hle/service/audio/errors.h"
#include "core/hle/service/bcat/bcat_result.h"
#include "core/hle/service/friend/friend_results.h"
#include "core/hle/service/ldn/ldn_results.h"
#include "core/hle/service/mii/mii_result.h"
#include "core/hle/service/nfc/nfc_result.h"
#include "core/hle/service/nfp/nfp_result.h"
#include "core/hle/service/ngc/ngc_results.h"
#include "core/hle/service/prepo/prepo_results.h"
#include "core/hle/service/psc/time/errors.h"
#include "core/hle/service/ro/ro_results.h"
#include "core/hle/service/ssl/ssl_backend.h"
#include "core/hle/service/ssl/ssl_results.h"
#include "core/hle/service/vi/vi_results.h"
#include "hid_core/hid_result.h"

// Every value pinned below is cross-checked against the Nintendo Switch SDK
// 15.3.2 (February 2023) header nn/<service>/<service>_Result.public.h.
// Nintendo generates those headers from each service's internal error table,
// so they publish the codes a guest can observe. This file guards the codes
// that were verified against them, so a later edit cannot silently change a
// value a game compares against.

TEST_CASE("LDN result codes match the SDK error table", "[core][result-codes][ldn]") {
    using namespace Service::LDN;

    // The SDK publishes the module as 203 and groups the codes into ranges:
    // device availability, library state, node lookup, connection,
    // cancellation, version compatibility and node count limits.
    REQUIRE(ResultDeviceNotAvailable.GetModule() == ErrorModule::LDN);
    REQUIRE(ResultDeviceNotAvailable.GetDescription() == 16);
    REQUIRE(ResultDeviceOccupied.GetDescription() == 17);
    REQUIRE(ResultDeviceDisabled.GetDescription() == 22);
    REQUIRE(ResultWifiOff.GetDescription() == 23);
    REQUIRE(ResultSleep.GetDescription() == 24);
    REQUIRE(ResultDeviceNotFound.GetDescription() == 25);
    REQUIRE(ResultInvalidState.GetDescription() == 32);
    REQUIRE(ResultNodeNotFound.GetDescription() == 48);
    REQUIRE(ResultConnectionFailed.GetDescription() == 64);
    REQUIRE(ResultConnectionTimeout.GetDescription() == 66);
    REQUIRE(ResultConnectionRejected.GetDescription() == 67);
    REQUIRE(ResultInvalidNetwork.GetDescription() == 68);
    REQUIRE(ResultCancelled.GetDescription() == 80);
    REQUIRE(ResultLowerVersion.GetDescription() == 113);
    REQUIRE(ResultHigherVersion.GetDescription() == 114);
    REQUIRE(ResultLowerVersionCannotUpdate.GetDescription() == 115);
    REQUIRE(ResultHigherVersionCannotUpdate.GetDescription() == 116);
    REQUIRE(ResultNodeCountLimitation.GetDescription() == 144);

    // A connection failure used to be reported as description 31, which the
    // SDK assigns to the device-unavailable family instead.
    REQUIRE(ResultConnectionFailed.GetModule() == ErrorModule::LDN);
    REQUIRE(ResultConnectionFailed.GetDescription() == 64);
}

TEST_CASE("Account result codes match the SDK error table", "[core][result-codes][acc]") {
    using namespace Service::Account;

    REQUIRE(ResultCancelled.GetDescription() == 0);
    REQUIRE(ResultCancelledByUser.GetDescription() == 1);
    REQUIRE(ResultUserNotExist.GetDescription() == 100);
    REQUIRE(ResultNetworkServiceAccountUnavailable.GetDescription() == 200);
    REQUIRE(ResultNintendoAccountAuthorizationInteractionRequired.GetDescription() == 280);
    REQUIRE(ResultTokenCacheUnavailable.GetDescription() == 430);
    REQUIRE(ResultNetworkCommunicationError.GetDescription() == 3000);
}

TEST_CASE("MII result codes match the SDK error table", "[core][result-codes][mii]") {
    using namespace Service::Mii;

    REQUIRE(ResultInvalidArgument.GetDescription() == 1);
    REQUIRE(ResultInvalidArgumentSize.GetDescription() == 2);
    REQUIRE(ResultNotFound.GetDescription() == 4);
    REQUIRE(ResultDatabaseFull.GetDescription() == 5);
    REQUIRE(ResultTestModeOnly.GetDescription() == 204);
    REQUIRE(ResultInvalidCharInfoType.GetDescription() == 205);
}

TEST_CASE("BCAT result codes match the SDK error table", "[core][result-codes][bcat]") {
    using namespace Service::BCAT;

    REQUIRE(ResultInvalidArgument.GetDescription() == 1);
    REQUIRE(ResultFailedOpenEntity.GetDescription() == 2);
    REQUIRE(ResultEntityAlreadyOpen.GetDescription() == 6);
    REQUIRE(ResultNoOpenEntry.GetDescription() == 7);
}

TEST_CASE("RO result codes match the SDK error table", "[core][result-codes][ro]") {
    using namespace Service::RO;

    REQUIRE(ResultOutOfAddressSpace.GetDescription() == 2);
    REQUIRE(ResultAlreadyLoaded.GetDescription() == 3);
    REQUIRE(ResultInvalidNro.GetDescription() == 4);
    REQUIRE(ResultInvalidNrr.GetDescription() == 6);
    REQUIRE(ResultTooManyNro.GetDescription() == 7);
    REQUIRE(ResultTooManyNrr.GetDescription() == 8);
    REQUIRE(ResultNotAuthorized.GetDescription() == 9);
    REQUIRE(ResultInternalError.GetDescription() == 1023);
}

TEST_CASE("VI result codes match the SDK error table", "[core][result-codes][vi]") {
    using namespace Service::VI;

    REQUIRE(ResultOperationFailed.GetDescription() == 1);
    REQUIRE(ResultPermissionDenied.GetDescription() == 5);
    REQUIRE(ResultNotSupported.GetDescription() == 6);
    REQUIRE(ResultNotFound.GetDescription() == 7);
}

TEST_CASE("Audio result codes match the SDK error table", "[core][result-codes][audio]") {
    using namespace Service::Audio;

    REQUIRE(ResultNotFound.GetDescription() == 1);
    REQUIRE(ResultOperationFailed.GetDescription() == 2);
    REQUIRE(ResultInvalidSampleRate.GetDescription() == 3);
    REQUIRE(ResultInsufficientBuffer.GetDescription() == 4);
    REQUIRE(ResultOutOfSessions.GetDescription() == 5);
    REQUIRE(ResultInvalidChannelCount.GetDescription() == 10);
    REQUIRE(ResultInvalidUpdateInfo.GetDescription() == 41);
    REQUIRE(ResultInvalidAddressInfo.GetDescription() == 42);
    REQUIRE(ResultNotSupported.GetDescription() == 513);
}

TEST_CASE("FS result codes match the SDK error table", "[core][result-codes][fs]") {
    using namespace FileSys;

    REQUIRE(ResultPathNotFound.GetModule() == ErrorModule::FS);
    REQUIRE(ResultPathNotFound.GetDescription() == 1);
    REQUIRE(ResultPathAlreadyExists.GetDescription() == 2);
    REQUIRE(ResultTargetLocked.GetDescription() == 7);
    REQUIRE(ResultDirectoryNotEmpty.GetDescription() == 8);
    REQUIRE(ResultDirectoryStatusChanged.GetDescription() == 13);
    REQUIRE(ResultUsableSpaceNotEnough.GetDescription() == 30);
    REQUIRE(ResultUsableSpaceNotEnoughForSaveData.GetDescription() == 31);
    REQUIRE(ResultUsableSpaceNotEnoughForCacheStorage.GetDescription() == 33);
    REQUIRE(ResultUnsupportedSdkVersion.GetDescription() == 50);
    REQUIRE(ResultMountNameAlreadyExists.GetDescription() == 60);
    REQUIRE(ResultIndividualFileDataCacheAlreadyEnabled.GetDescription() == 70);
    REQUIRE(ResultPartitionNotFound.GetDescription() == 1001);
    REQUIRE(ResultTargetNotFound.GetDescription() == 1002);
    REQUIRE(ResultNcaExternalKeyUnavailable.GetDescription() == 1004);
    REQUIRE(ResultSdCardAccessFailed.GetDescription() == 2000);
    REQUIRE(ResultGameCardAccessFailed.GetDescription() == 2500);
    REQUIRE(ResultGameCardCardAccessFailure.GetDescription() == 2530);
    REQUIRE(ResultGameCardInitializeAsicFailure.GetDescription() == 2629);
    REQUIRE(ResultNotImplemented.GetDescription() == 3001);
    REQUIRE(ResultUnsupportedVersion.GetDescription() == 3002);
    REQUIRE(ResultAlreadyExists.GetDescription() == 3003);
    REQUIRE(ResultTargetLockedWithAbort.GetDescription() == 3004);
    REQUIRE(ResultOutOfRange.GetDescription() == 3005);
    REQUIRE(ResultDeferredProcessRetryFailure.GetDescription() == 3006);
    REQUIRE(ResultAllocationMemoryFailed.GetDescription() == 3200);
    REQUIRE(ResultMmcAccessFailed.GetDescription() == 3500);
    REQUIRE(ResultDataCorrupted.GetDescription() == 4000);
    REQUIRE(ResultRomHostFileSystemCorrupted.GetDescription() == 4241);
    REQUIRE(ResultRomHostEntryCorrupted.GetDescription() == 4242);
    REQUIRE(ResultRomHostFileDataCorrupted.GetDescription() == 4243);
    REQUIRE(ResultRomHostFileCorrupted.GetDescription() == 4244);
    REQUIRE(ResultInvalidRomHostHandle.GetDescription() == 4245);
    REQUIRE(ResultSaveDataHostFileSystemCorrupted.GetDescription() == 4441);
    REQUIRE(ResultSaveDataHostEntryCorrupted.GetDescription() == 4442);
    REQUIRE(ResultSaveDataHostFileDataCorrupted.GetDescription() == 4443);
    REQUIRE(ResultSaveDataHostFileCorrupted.GetDescription() == 4444);
    REQUIRE(ResultSaveDataInvalidHostHandle.GetDescription() == 4445);
    REQUIRE(ResultHostFileSystemCorrupted.GetDescription() == 4701);
    REQUIRE(ResultHostEntryCorrupted.GetDescription() == 4702);
    REQUIRE(ResultHostFileDataCorrupted.GetDescription() == 4703);
    REQUIRE(ResultHostFileCorrupted.GetDescription() == 4704);
    REQUIRE(ResultInvalidHostHandle.GetDescription() == 4705);
    REQUIRE(ResultGameCardLogoDataCorrupted.GetDescription() == 4781);
    REQUIRE(ResultUnexpected.GetDescription() == 5000);
}

TEST_CASE("NFP result codes match the SDK error table", "[core][result-codes][nfp]") {
    using namespace Service::NFP;

    REQUIRE(ResultNfcDeviceNotFound.GetModule() == ErrorModule::NFP);
    REQUIRE(ResultNfcDeviceNotFound.GetDescription() == 64);
    REQUIRE(ResultDeviceNotFound.GetDescription() == 64);
    REQUIRE(ResultNfcDisabled.GetDescription() == 80);
    REQUIRE(ResultNeedRetry.GetDescription() == 88);
    REQUIRE(ResultWriteAmiiboFailed.GetDescription() == 88);
    REQUIRE(ResultNeedRestart.GetDescription() == 96);
    REQUIRE(ResultTagRemoved.GetDescription() == 97);
    REQUIRE(ResultNeedRegister.GetDescription() == 120);
    REQUIRE(ResultRegistrationIsNotInitialized.GetDescription() == 120);
    REQUIRE(ResultNeedCreate.GetDescription() == 128);
    REQUIRE(ResultApplicationAreaIsNotInitialized.GetDescription() == 128);
    REQUIRE(ResultNeedRestore.GetDescription() == 136);
    REQUIRE(ResultCorruptedDataWithBackup.GetDescription() == 136);
    REQUIRE(ResultNeedFormat.GetDescription() == 144);
    REQUIRE(ResultCorruptedData.GetDescription() == 144);
    REQUIRE(ResultAccessIdMisMatch.GetDescription() == 152);
    REQUIRE(ResultWrongApplicationAreaId.GetDescription() == 152);
    REQUIRE(ResultNotBroken.GetDescription() == 160);
    REQUIRE(ResultAlreadyCreated.GetDescription() == 168);
    REQUIRE(ResultApplicationAreaExist.GetDescription() == 168);
    REQUIRE(ResultNotSupported.GetDescription() == 176);
    REQUIRE(ResultNotAnAmiibo.GetDescription() == 178);
    REQUIRE(ResultInvalidFormatVersion.GetDescription() == 184);
    REQUIRE(ResultMaxNfcDeviceActivated.GetDescription() == 192);
    REQUIRE(ResultConflictFunction.GetDescription() == 200);
    REQUIRE(ResultUnableToAccessBackupFile.GetDescription() == 200);
    REQUIRE(ResultNotUpdated.GetDescription() == 208);
}

TEST_CASE("NFC result codes match the SDK error table", "[core][result-codes][nfc]") {
    using namespace Service::NFC;

    REQUIRE(ResultNfcDeviceNotFound.GetModule() == ErrorModule::NFC);
    REQUIRE(ResultNfcDeviceNotFound.GetDescription() == 64);
    REQUIRE(ResultDeviceNotFound.GetDescription() == 64);
    REQUIRE(ResultNfcDisabled.GetDescription() == 80);
    REQUIRE(ResultNeedRestart.GetDescription() == 96);
    REQUIRE(ResultTagRemoved.GetDescription() == 97);
    REQUIRE(ResultNotSupported.GetDescription() == 176);
    REQUIRE(ResultInvalidTagType.GetDescription() == 178);
    REQUIRE(ResultMaxNfcDeviceActivated.GetDescription() == 192);
    REQUIRE(ResultConflictFunction.GetDescription() == 200);
    REQUIRE(ResultAccessError.GetDescription() == 288);
    REQUIRE(ResultMifareError288.GetDescription() == 288);
}

TEST_CASE("HID result codes match the SDK error table", "[core][result-codes][hid]") {
    using namespace Service::HID;

    REQUIRE(ResultVibrationFileInvalid.GetModule() == ErrorModule::HID);
    REQUIRE(ResultVibrationFileInvalid.GetDescription() == 140);
    REQUIRE(ResultNpadError.GetDescription() == 600);
    REQUIRE(ResultNpadControllerNotConnected.GetDescription() == 604);
}

TEST_CASE("IRSensor result codes match the SDK error table", "[core][result-codes][irsensor]") {
    using namespace Service::IRS;

    REQUIRE(ResultIrsensorUnavailable.GetModule() == ErrorModule::Irsensor);
    REQUIRE(ResultIrsensorUnavailable.GetDescription() == 110);
    REQUIRE(ResultIrsensorUnconnected.GetDescription() == 110);
    REQUIRE(ResultIrsensorUnsupported.GetDescription() == 111);
    REQUIRE(ResultIrsensorDeviceError.GetDescription() == 122);
    REQUIRE(ResultIrsensorFirmwareCheckIncompleted.GetDescription() == 150);
    REQUIRE(ResultIrsensorNotReady.GetDescription() == 160);
    REQUIRE(ResultIrsensorDeviceNotReady.GetDescription() == 160);
    REQUIRE(ResultIrsensorDeviceResourceNotAvailable.GetDescription() == 161);
    REQUIRE(ResultHandAnalysisError.GetDescription() == 1100);
    REQUIRE(ResultHandAnalysisModeIncorrect.GetDescription() == 1101);
}

TEST_CASE("Time result codes match the SDK error table", "[core][result-codes][time]") {
    using namespace Service::PSC::Time;

    REQUIRE(ResultNotInitialized.GetModule() == ErrorModule::TimeService);
    REQUIRE(ResultNotInitialized.GetDescription() == 0);
    REQUIRE(ResultNoCapability.GetDescription() == 1);
    REQUIRE(ResultPermissionDenied.GetDescription() == 1);
    REQUIRE(ResultClockInvalid.GetDescription() == 100);
    REQUIRE(ResultClockMismatch.GetDescription() == 102);
    REQUIRE(ResultClockUninitialized.GetDescription() == 103);
    REQUIRE(ResultNotComparable.GetDescription() == 200);
    REQUIRE(ResultTimeNotFound.GetDescription() == 200);
    REQUIRE(ResultOverflowed.GetDescription() == 201);
    REQUIRE(ResultOverflow.GetDescription() == 201);
    REQUIRE(ResultOutOfResource.GetDescription() == 800);
    REQUIRE(ResultOutOfMemory.GetDescription() == 801);
    REQUIRE(ResultFailed.GetDescription() == 801);
}

TEST_CASE("SSL result codes match the SDK error table", "[core][result-codes][ssl]") {
    using namespace Service::SSL;

    REQUIRE(ResultSslService.GetModule() == ErrorModule::SSLSrv);
    REQUIRE(ResultSslService.GetDescription() == 0);
    REQUIRE(Service::SSL::ResultUnknown.GetDescription() == 1);
    REQUIRE(ResultFatal.GetDescription() == 9);
    REQUIRE(ResultFatail.GetDescription() == 10);
    REQUIRE(ResultErrorLower.GetDescription() == 11);
    REQUIRE(ResultInvalidReference.GetDescription() == 12);
    REQUIRE(ResultInternalLogicError.GetDescription() == 13);

    REQUIRE(ResultRetryRequired.GetDescription() == 100);
    REQUIRE(ResultLibraryNotInitialized.GetDescription() == 101);
    REQUIRE(ResultInsufficientMemory.GetDescription() == 102);
    REQUIRE(ResultSocketNotRegistered.GetDescription() == 103);
    REQUIRE(ResultNoSocket.GetDescription() == 103);
    REQUIRE(ResultHostNameNotRegistered.GetDescription() == 104);
    REQUIRE(ResultHostNameTooLong.GetDescription() == 105);
    REQUIRE(ResultBufferAlreadyRegistered.GetDescription() == 106);
    REQUIRE(ResultConnectionRemaining.GetDescription() == 107);
    REQUIRE(ResultContextRemaining.GetDescription() == 108);
    REQUIRE(ResultInvalidConnectionContext.GetDescription() == 109);
    REQUIRE(ResultInvalidContext.GetDescription() == 110);
    REQUIRE(ResultInvalidIoMode.GetDescription() == 111);
    REQUIRE(ResultBufferTooShort.GetDescription() == 112);
    REQUIRE(ResultInvalidPointer.GetDescription() == 113);
    REQUIRE(ResultResourceMax.GetDescription() == 114);
    REQUIRE(ResultResourceBusy.GetDescription() == 115);
    REQUIRE(ResultInvalidSocketDescriptor.GetDescription() == 116);
    REQUIRE(ResultInvalidCertFormat.GetDescription() == 117);
    REQUIRE(ResultInvalidPasswordSize.GetDescription() == 118);
    REQUIRE(ResultNoTcpConnection.GetDescription() == 119);
    REQUIRE(ResultInvalidInternalPkiType.GetDescription() == 120);
    REQUIRE(ResultInvalidSessionCacheMode.GetDescription() == 121);
    REQUIRE(ResultInvalidRenegotiationMode.GetDescription() == 122);
    REQUIRE(ResultInvalidConcurrencyLimit.GetDescription() == 123);
    REQUIRE(ResultPolicyOidStringTooLong.GetDescription() == 124);
    REQUIRE(ResultInvalidPolicyOidStringBufferLength.GetDescription() == 125);
    REQUIRE(ResultInvalidOptionType.GetDescription() == 126);
    REQUIRE(ResultInvalidIdCount.GetDescription() == 127);
    REQUIRE(ResultInvalidIndex.GetDescription() == 128);
    REQUIRE(ResultInvalidCrlFormat.GetDescription() == 129);
    REQUIRE(ResultUnknownCrlIssuer.GetDescription() == 130);
    REQUIRE(ResultCrlSignatureVerifyFail.GetDescription() == 131);
    REQUIRE(ResultInvalidArgument.GetDescription() == 132);
    REQUIRE(ResultOperationNotAllowed.GetDescription() == 133);
    REQUIRE(ResultCommonNameTooLong.GetDescription() == 134);

    REQUIRE(ResultReport.GetDescription() == 200);
    REQUIRE(ResultLibraryAlreadyInitialized.GetDescription() == 201);
    REQUIRE(ResultInsufficientServerCertBuffer.GetDescription() == 202);
    REQUIRE(ResultSocketAlreadyRegistered.GetDescription() == 203);
    REQUIRE(ResultIoWouldBlock.GetDescription() == 204);
    REQUIRE(ResultWouldBlock.GetDescription() == 204);
    REQUIRE(ResultIoTimeout.GetDescription() == 205);
    REQUIRE(ResultTimeout.GetDescription() == 205);
    REQUIRE(ResultIoAborted.GetDescription() == 206);
    REQUIRE(ResultVerifyCertFailed.GetDescription() == 207);
    REQUIRE(ResultNoConnection.GetDescription() == 208);
    REQUIRE(ResultConnectionReset.GetDescription() == 209);
    REQUIRE(ResultConnectionAborted.GetDescription() == 210);
    REQUIRE(ResultSocketShutdown.GetDescription() == 211);
    REQUIRE(ResultNetworkDown.GetDescription() == 212);
    REQUIRE(ResultInvalidCertificate.GetDescription() == 213);
    REQUIRE(ResultInvalidCertStoreId.GetDescription() == 214);
    REQUIRE(ResultClientPkiAlreadyRegistered.GetDescription() == 215);
    REQUIRE(ResultNoSslConnection.GetDescription() == 216);
    REQUIRE(ResultConnectionClosed.GetDescription() == 217);
    REQUIRE(ResultMaxServerPkiRegistered.GetDescription() == 218);
    REQUIRE(ResultMaxPolicyOidRegistered.GetDescription() == 219);
    REQUIRE(ResultSslCoreInitializing.GetDescription() == 220);
    REQUIRE(ResultNoServerChain.GetDescription() == 221);
    REQUIRE(ResultClientPkiNotRegistered.GetDescription() == 222);
    REQUIRE(ResultNotInDtlsMode.GetDescription() == 223);
    REQUIRE(ResultNotPerformingHandshake.GetDescription() == 224);
    REQUIRE(ResultAllocatorAlreadySet.GetDescription() == 225);

    REQUIRE(ResultSslError.GetDescription() == 300);
    REQUIRE(ResultSslErrorInvalidCertDomain.GetDescription() == 301);
    REQUIRE(ResultSslErrorNoCertificate.GetDescription() == 302);
    REQUIRE(ResultSslErrorExpiredCertificate.GetDescription() == 303);
    REQUIRE(ResultSslErrorRevokedCertificate.GetDescription() == 304);
    REQUIRE(ResultSslErrorUnsupportedCertificate.GetDescription() == 305);
    REQUIRE(ResultSslErrorUnknownCertificate.GetDescription() == 306);
    REQUIRE(ResultSslErrorBadCertificate.GetDescription() == 307);
    REQUIRE(ResultSslErrorUnkownCa.GetDescription() == 308);
    REQUIRE(ResultSslErrorAccessDenied.GetDescription() == 309);
    REQUIRE(ResultSslErrorReceivedUnkownData.GetDescription() == 310);
    REQUIRE(ResultSslErrorReceivedMalformedData.GetDescription() == 311);
    REQUIRE(ResultSslErrorReceivedUnexpectedHandshakeData.GetDescription() == 312);
    REQUIRE(ResultSslErrorReceivedUnexpectedRecordData.GetDescription() == 313);
    REQUIRE(ResultSslErrorReceivedTooLongRecord.GetDescription() == 314);
    REQUIRE(ResultSslErrorUnexpectedHelloVerifyRequest.GetDescription() == 315);
    REQUIRE(ResultSslErrorUnexpectedCertificateStatusResponse.GetDescription() == 316);
    REQUIRE(ResultSslErrorInconsistentSignAlgorithm.GetDescription() == 317);
    REQUIRE(ResultSslErrorInsufficientSecurity.GetDescription() == 318);
    REQUIRE(ResultSslErrorDecryptionWithInvalidMac.GetDescription() == 319);
    REQUIRE(ResultSslErrorDigestCalculationFailed.GetDescription() == 321);
    REQUIRE(ResultSslErrorMacCalculationFailed.GetDescription() == 322);
    REQUIRE(ResultSslErrorClientCertificateNotFound.GetDescription() == 323);
    REQUIRE(ResultSslErrorNoExtendedField.GetDescription() == 324);
    REQUIRE(ResultSslErrorUntrustedOid.GetDescription() == 325);
    REQUIRE(ResultSslErrorInvalidPolicy.GetDescription() == 326);
    REQUIRE(ResultSslErrorNoCipher.GetDescription() == 327);
    REQUIRE(ResultSslErrorUnknownEvFingerprint.GetDescription() == 328);
    REQUIRE(ResultSslErrorDecompressionFailed.GetDescription() == 330);

    REQUIRE(ResultSslAlert.GetDescription() == 1500);
    REQUIRE(ResultSslAlertCloseNotify.GetDescription() == 1501);
    REQUIRE(ResultSslAlertUnexpectedMessage.GetDescription() == 1502);
    REQUIRE(ResultSslAlertBadRecordMac.GetDescription() == 1503);
    REQUIRE(ResultSslAlertDecriptionFailed.GetDescription() == 1504);
    REQUIRE(ResultSslAlertRecordOverfrlow.GetDescription() == 1505);
    REQUIRE(ResultSslAlertDecompressionFailure.GetDescription() == 1506);
    REQUIRE(ResultSslAlertHandshakeFailure.GetDescription() == 1507);
    REQUIRE(ResultSslAlertNoCertificate.GetDescription() == 1508);
    REQUIRE(ResultSslAlertBadCertificate.GetDescription() == 1509);
    REQUIRE(ResultSslAlertUnsupportedCertificate.GetDescription() == 1510);
    REQUIRE(ResultSslAlertCertificateRevoked.GetDescription() == 1511);
    REQUIRE(ResultSslAlertCertificateExpired.GetDescription() == 1512);
    REQUIRE(ResultSslAlertCertificateUnkown.GetDescription() == 1513);
    REQUIRE(ResultSslAlertIllegalParameter.GetDescription() == 1514);
    REQUIRE(ResultSslAlertUnknownCa.GetDescription() == 1515);
    REQUIRE(ResultSslAlertAccessDenied.GetDescription() == 1516);
    REQUIRE(ResultSslAlertDecodeError.GetDescription() == 1517);
    REQUIRE(ResultSslAlertDecryptError.GetDescription() == 1518);
    REQUIRE(ResultSslAlertExportRestriction.GetDescription() == 1519);
    REQUIRE(ResultSslAlertProtocolVersion.GetDescription() == 1520);
    REQUIRE(ResultSslAlertInsufficientSecurity.GetDescription() == 1521);
    REQUIRE(ResultSslAlertInternalError.GetDescription() == 1522);
    REQUIRE(ResultSslAlertInappropriateFallback.GetDescription() == 1523);
    REQUIRE(ResultSslAlertUserCancelled.GetDescription() == 1524);
    REQUIRE(ResultSslAlertNoRenegotiation.GetDescription() == 1525);
    REQUIRE(ResultSslAlertUnsupportedExtention.GetDescription() == 1526);
    REQUIRE(ResultSslAlertCertificateUnobtainable.GetDescription() == 1527);
    REQUIRE(ResultSslAlertUnrecognizedName.GetDescription() == 1528);
    REQUIRE(ResultSslAlertBadCertificateStatusResponse.GetDescription() == 1529);
    REQUIRE(ResultSslAlertBadCertificateHashValue.GetDescription() == 1530);
    REQUIRE(ResultSslAlertUnkownPskIdentity.GetDescription() == 1531);
    REQUIRE(ResultSslAlertNoApplicationProtocol.GetDescription() == 1532);
}

TEST_CASE("Friends result codes match the SDK error table", "[core][result-codes][friends]") {
    using namespace Service::Friend;

    REQUIRE(ResultNotInitialized.GetModule() == ErrorModule::Friends);
    REQUIRE(ResultNotInitialized.GetDescription() == 1);
    REQUIRE(ResultInvalidArgument.GetDescription() == 2);
    REQUIRE(ResultUserNotOpened.GetDescription() == 3);
    REQUIRE(ResultNetworkServiceAccountNotLinked.GetDescription() == 4);
    REQUIRE(ResultOwnNetworkServiceAccountSpecified.GetDescription() == 5);
    REQUIRE(ResultInternetRequestNotAccepted.GetDescription() == 6);
    REQUIRE(ResultNotCalled.GetDescription() == 7);
    REQUIRE(ResultCallInProgress.GetDescription() == 8);
    REQUIRE(ResultCanceled.GetDescription() == 9);
    REQUIRE(ResultProfileImageCacheNotFound.GetDescription() == 10);
    REQUIRE(ResultOutOfMemory.GetDescription() == 11);
    REQUIRE(ResultOutOfResource.GetDescription() == 12);
    REQUIRE(ResultReservedKey.GetDescription() == 13);
    REQUIRE(ResultDuplicatedKey.GetDescription() == 14);
    REQUIRE(ResultNotificationNotFound.GetDescription() == 15);
    REQUIRE(ResultPlayHistoryRegistrationKeyBroken.GetDescription() == 21);
    REQUIRE(ResultOwnPlayHistoryRegistrationKey.GetDescription() == 22);
    REQUIRE(ResultAppletCanceled.GetDescription() == 30);
    REQUIRE(ResultApplicationInfoNotRegistered.GetDescription() == 40);
    REQUIRE(ResultNotPermitted.GetDescription() == 90);
    REQUIRE(ResultInvalidOperation.GetDescription() == 92);
    REQUIRE(ResultNotImplemented.GetDescription() == 99);
    REQUIRE(ResultResponseFormatError.GetDescription() == 100);
    REQUIRE(ResultHttpError.GetDescription() == 1000);
    REQUIRE(ResultServerError.GetDescription() == 2000);
}

TEST_CASE("Prepo result codes match the SDK error table", "[core][result-codes][prepo]") {
    using namespace Service::PlayReport;

    REQUIRE(ResultInvalidArgument.GetModule() == ErrorModule::PlayReport);
    REQUIRE(ResultInvalidArgument.GetDescription() == 1);
    REQUIRE(ResultInvalidKey.GetDescription() == 2);
    REQUIRE(ResultDuplicatedKey.GetDescription() == 3);
    REQUIRE(ResultInvalidValue.GetDescription() == 4);
    REQUIRE(ResultInvalidEventId.GetDescription() == 5);
    REQUIRE(ResultOutOfResource.GetDescription() == 6);
    REQUIRE(ResultKeyValueCountLimitReached.GetDescription() == 7);
    REQUIRE(ResultInvalidApplicationId.GetDescription() == 8);
    REQUIRE(ResultInvalidReportData.GetDescription() == 9);
    REQUIRE(ResultTooBigReportData.GetDescription() == 10);
    REQUIRE(ResultTransmissionNotAgreed.GetDescription() == 11);
    REQUIRE(ResultNetworkUnavailable.GetDescription() == 12);
    REQUIRE(ResultInProgress.GetDescription() == 60);
    REQUIRE(ResultCanceled.GetDescription() == 61);
    REQUIRE(ResultNotPermitted.GetDescription() == 90);
    REQUIRE(ResultNotImplemented.GetDescription() == 99);
}

TEST_CASE("NGC result codes match the SDK error table", "[core][result-codes][ngc]") {
    using namespace Service::NGC;

    REQUIRE(ResultNotInitialized.GetModule() == ErrorModule::NGC);
    REQUIRE(ResultNotInitialized.GetDescription() == 1);
    REQUIRE(ResultAlreadyInitialized.GetDescription() == 2);
    REQUIRE(ResultInvalidPointer.GetDescription() == 3);
    REQUIRE(ResultInvalidSize.GetDescription() == 4);
    REQUIRE(ResultInvalidCharacter.GetDescription() == 5);
}

TEST_CASE("OE result codes match the SDK error table", "[core][result-codes][oe]") {
    using namespace Service::OE;

    REQUIRE(ResultInvalidConfiguration.GetModule() == ErrorModule::OE);
    REQUIRE(ResultInvalidConfiguration.GetDescription() == 1);
    REQUIRE(ResultNotAllowed.GetDescription() == 11);
    REQUIRE(ResultInternalError.GetDescription() == 1023);
    REQUIRE(Service::OE::ResultUnknown.GetDescription() == 1024);
}
