/** @file
  This module install ACPI Boot Graphics Resource Table (BGRT).

  Copyright (c) 2011 - 2018, Intel Corporation. All rights reserved.<BR>
  This program and the accompanying materials
  are licensed and made available under the terms and conditions of the BSD License
  which accompanies this distribution.  The full text of the license may be found at
  http://opensource.org/licenses/bsd-license.php

  THE PROGRAM IS DISTRIBUTED UNDER THE BSD LICENSE ON AN "AS IS" BASIS,
  WITHOUT WARRANTIES OR REPRESENTATIONS OF ANY KIND, EITHER EXPRESS OR IMPLIED.
**/

#include <Uefi.h>

#include <IndustryStandard/Acpi.h>

#include <Protocol/AcpiTable.h>
#include <Protocol/GraphicsOutput.h>
#include <Protocol/BootLogo.h>

#include <Guid/EventGroup.h>

#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/DebugLib.h>
#include <Library/PcdLib.h>
#include <Library/SafeIntLib.h>
#include <Library/BmpSupportLib.h>

/**
  Update information of logo image drawn on screen.

  @param  This           The pointer to the Boot Logo protocol instance.
  @param  BltBuffer      The BLT buffer for logo drawn on screen. If BltBuffer
                         is set to NULL, it indicates that logo image is no
                         longer on the screen.
  @param  DestinationX   X coordinate of destination for the BltBuffer.
  @param  DestinationY   Y coordinate of destination for the BltBuffer.
  @param  Width          Width of rectangle in BltBuffer in pixels.
  @param  Height         Hight of rectangle in BltBuffer in pixels.

  @retval EFI_SUCCESS             The boot logo information was updated.
  @retval EFI_INVALID_PARAMETER   One of the parameters has an invalid value.
  @retval EFI_OUT_OF_RESOURCES    The logo information was not updated due to
                                  insufficient memory resources.

**/
EFI_STATUS
EFIAPI
SetBootLogo (
  IN EFI_BOOT_LOGO_PROTOCOL         *This,
  IN EFI_GRAPHICS_OUTPUT_BLT_PIXEL  *BltBuffer       OPTIONAL,
  IN UINTN                          DestinationX,
  IN UINTN                          DestinationY,
  IN UINTN                          Width,
  IN UINTN                          Height
  );

//
// Boot Logo Protocol Handle
//
EFI_HANDLE  mBootLogoHandle = NULL;

//
// Boot Logo Protocol Instance
//
EFI_BOOT_LOGO_PROTOCOL  mBootLogoProtocolTemplate = {
  SetBootLogo
};

EFI_EVENT                      mBootGraphicsReadyToBootEvent;
UINTN                          mBootGraphicsResourceTableKey = 0;
BOOLEAN                        mIsLogoValid = FALSE;
EFI_GRAPHICS_OUTPUT_BLT_PIXEL  *mLogoBltBuffer = NULL;
UINTN                          mLogoDestX  = 0;
UINTN                          mLogoDestY  = 0;
UINTN                          mLogoWidth  = 0;
UINTN                          mLogoHeight = 0;
BOOLEAN                        mAcpiBgrtInstalled     = FALSE;
BOOLEAN                        mAcpiBgrtStatusChanged = FALSE;
BOOLEAN                        mAcpiBgrtBufferChanged = FALSE;

//
// ACPI Boot Graphics Resource Table template
//
EFI_ACPI_5_0_BOOT_GRAPHICS_RESOURCE_TABLE mBootGraphicsResourceTableTemplate = {
  {
    EFI_ACPI_5_0_BOOT_GRAPHICS_RESOURCE_TABLE_SIGNATURE,
    sizeof (EFI_ACPI_5_0_BOOT_GRAPHICS_RESOURCE_TABLE),
    EFI_ACPI_5_0_BOOT_GRAPHICS_RESOURCE_TABLE_REVISION,     // Revision
    0x00,  // Checksum will be updated at runtime
    //
    // It is expected that these values will be updated at EntryPoint.
    //
    {0x00},     // OEM ID is a 6 bytes long field
    0x00,       // OEM Table ID(8 bytes long)
    0x00,       // OEM Revision
    0x00,       // Creator ID
    0x00,       // Creator Revision
  },
  EFI_ACPI_5_0_BGRT_VERSION,         // Version
  EFI_ACPI_5_0_BGRT_STATUS_VALID,    // Status
  EFI_ACPI_5_0_BGRT_IMAGE_TYPE_BMP,  // Image Type
  0,                                 // Image Address
  0,                                 // Image Offset X
  0                                  // Image Offset Y
};

/**
  Update information of logo image drawn on screen.

  @param  This           The pointer to the Boot Logo protocol instance.
  @param  BltBuffer      The BLT buffer for logo drawn on screen. If BltBuffer
                         is set to NULL, it indicates that logo image is no
                         longer on the screen.
  @param  DestinationX   X coordinate of destination for the BltBuffer.
  @param  DestinationY   Y coordinate of destination for the BltBuffer.
  @param  Width          Width of rectangle in BltBuffer in pixels.
  @param  Height         Hight of rectangle in BltBuffer in pixels.

  @retval EFI_SUCCESS             The boot logo information was updated.
  @retval EFI_INVALID_PARAMETER   One of the parameters has an invalid value.
  @retval EFI_OUT_OF_RESOURCES    The logo information was not updated due to
                                  insufficient memory resources.

**/
EFI_STATUS
EFIAPI
SetBootLogo (
  IN EFI_BOOT_LOGO_PROTOCOL            *This,
  IN EFI_GRAPHICS_OUTPUT_BLT_PIXEL     *BltBuffer       OPTIONAL,
  IN UINTN                             DestinationX,
  IN UINTN                             DestinationY,
  IN UINTN                             Width,
  IN UINTN                             Height
  )
{
  EFI_STATUS  Status;
  UINTN       BufferSize;
  UINT32      Result32;

  if (BltBuffer == NULL) {
    mIsLogoValid = FALSE;
    mAcpiBgrtStatusChanged = TRUE;
    return EFI_SUCCESS;
  }

  //
  // Width and height are not allowed to be zero.
  //
  if (Width == 0 || Height == 0) {
    return EFI_INVALID_PARAMETER;
  }

  //
  // Verify destination, width, and height do not overflow 32-bit values.
  // The Boot Graphics Resource Table only has 32-bit fields for these values.
  //
  Status = SafeUintnToUint32 (DestinationX, &Result32);
  if (EFI_ERROR (Status)) {
    return EFI_INVALID_PARAMETER;
  }
  Status = SafeUintnToUint32 (DestinationY, &Result32);
  if (EFI_ERROR (Status)) {
    return EFI_INVALID_PARAMETER;
  }
  Status = SafeUintnToUint32 (Width, &Result32);
  if (EFI_ERROR (Status)) {
    return EFI_INVALID_PARAMETER;
  }
  Status = SafeUintnToUint32 (Height, &Result32);
  if (EFI_ERROR (Status)) {
    return EFI_INVALID_PARAMETER;
  }

  //
  // Ensure the Height * Width * sizeof (EFI_GRAPHICS_OUTPUT_BLT_PIXEL) does
  // not overflow UINTN
  //
  Status = SafeUintnMult (
             Width,
             Height,
             &BufferSize
             );
  if (EFI_ERROR (Status)) {
    return EFI_UNSUPPORTED;
  }
  Status = SafeUintnMult (
             BufferSize,
             sizeof (EFI_GRAPHICS_OUTPUT_BLT_PIXEL),
             &BufferSize
             );
  if (EFI_ERROR (Status)) {
    return EFI_UNSUPPORTED;
  }

  //
  // Update state
  //
  mAcpiBgrtBufferChanged = TRUE;

  //
  // Free old logo buffer
  //
  if (mLogoBltBuffer != NULL) {
    FreePool (mLogoBltBuffer);
    mLogoBltBuffer = NULL;
  }

  //
  // Allocate new logo buffer
  //
  mLogoBltBuffer = AllocateCopyPool (BufferSize, BltBuffer);
  if (mLogoBltBuffer == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  mLogoDestX   = DestinationX;
  mLogoDestY   = DestinationY;
  mLogoWidth   = Width;
  mLogoHeight  = Height;
  mIsLogoValid = TRUE;

  return EFI_SUCCESS;
}

/**
  Notify function for event group EFI_EVENT_GROUP_READY_TO_BOOT. This is used to
  install the Boot Graphics Resource Table.

  @param[in]  Event   The Event that is being processed.
  @param[in]  Context The Event Context.

**/
VOID
EFIAPI
BgrtReadyToBootEventNotify (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  EFI_STATUS               Status;
  EFI_ACPI_TABLE_PROTOCOL  *AcpiTableProtocol;
  VOID                     *ImageBuffer;
  UINT32                   BmpSize;

  //
  // Get ACPI Table protocol.
  //
  Status = gBS->LocateProtocol (
                  &gEfiAcpiTableProtocolGuid,
                  NULL,
                  (VOID **) &AcpiTableProtocol
                  );
  if (EFI_ERROR (Status)) {
    return;
  }

  //
  // Check whether Boot Graphics Resource Table is already installed.
  //
  if (mAcpiBgrtInstalled) {
    if (!mAcpiBgrtStatusChanged && !mAcpiBgrtBufferChanged) {
      //
      // Nothing has changed
      //
      return;
    } else {
      //
      // If BGRT data change happens, then uninstall orignal AcpiTable first
      //
      Status = AcpiTableProtocol->UninstallAcpiTable (
                                    AcpiTableProtocol,
                                    mBootGraphicsResourceTableKey
                                    );
      if (EFI_ERROR (Status)) {
        return;
      }
    }
  } else {
    //
    // Check whether Logo exists
    //
    if (mLogoBltBuffer == NULL) {
      return;
    }
  }

  if (mAcpiBgrtBufferChanged) {
    //
    // Free the old BMP image buffer
    //
    ImageBuffer = (UINT8 *)(UINTN)mBootGraphicsResourceTableTemplate.ImageAddress;
    if (ImageBuffer != NULL) {
      FreePool (ImageBuffer);
    }

    //
    // Convert GOP Blt buffer to BMP image.  Pass in ImageBuffer set to NULL
    // so the BMP image is allocated by TranslateGopBltToBmp().
    //
    ImageBuffer = NULL;
    Status = TranslateGopBltToBmp (
               mLogoBltBuffer,
               (UINT32)mLogoHeight,
               (UINT32)mLogoWidth,
               &ImageBuffer,
               &BmpSize
               );
    if (EFI_ERROR (Status)) {
      return;
    }

    //
    // Free the logo buffer
    //
    FreePool (mLogoBltBuffer);
    mLogoBltBuffer = NULL;

    //
    // Update BMP image fields of the Boot Graphics Resource Table
    //
    mBootGraphicsResourceTableTemplate.ImageAddress = (UINT64)(UINTN)ImageBuffer;
    mBootGraphicsResourceTableTemplate.ImageOffsetX = (UINT32)mLogoDestX;
    mBootGraphicsResourceTableTemplate.ImageOffsetY = (UINT32)mLogoDestY;
  }

  //
  // Update Status field of Boot Graphics Resource Table
  //
  if (mIsLogoValid) {
    mBootGraphicsResourceTableTemplate.Status = EFI_ACPI_5_0_BGRT_STATUS_VALID;
  } else {
    mBootGraphicsResourceTableTemplate.Status = EFI_ACPI_5_0_BGRT_STATUS_INVALID;
  }

  //
  // Update Checksum of Boot Graphics Resource Table
  //
  mBootGraphicsResourceTableTemplate.Header.Checksum = 0;
  mBootGraphicsResourceTableTemplate.Header.Checksum =
    CalculateCheckSum8 (
      (UINT8 *)&mBootGraphicsResourceTableTemplate,
      sizeof (EFI_ACPI_5_0_BOOT_GRAPHICS_RESOURCE_TABLE)
      );

  //
  // Publish Boot Graphics Resource Table.
  //
  Status = AcpiTableProtocol->InstallAcpiTable (
                                AcpiTableProtocol,
                                &mBootGraphicsResourceTableTemplate,
                                sizeof (EFI_ACPI_5_0_BOOT_GRAPHICS_RESOURCE_TABLE),
                                &mBootGraphicsResourceTableKey
                                );
  if (EFI_ERROR (Status)) {
    return;
  }

  mAcpiBgrtInstalled     = TRUE;
  mAcpiBgrtStatusChanged = FALSE;
  mAcpiBgrtBufferChanged = FALSE;
}

/**
  The module Entry Point of the Boot Graphics Resource Table DXE driver.

  @param[in]  ImageHandle    The firmware allocated handle for the EFI image.
  @param[in]  SystemTable    A pointer to the EFI System Table.

  @retval EFI_SUCCESS    The entry point is executed successfully.
  @retval Other          Some error occurs when executing this entry point.

**/
EFI_STATUS
EFIAPI
BootGraphicsDxeEntryPoint (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS                   Status;
  EFI_ACPI_DESCRIPTION_HEADER  *Header;

  //
  // Update Header fields of Boot Graphics Resource Table from PCDs
  //
  Header = &mBootGraphicsResourceTableTemplate.Header;
  ZeroMem (Header->OemId, sizeof (Header->OemId));
  CopyMem (
    Header->OemId,
    PcdGetPtr (PcdAcpiDefaultOemId),
    MIN (PcdGetSize (PcdAcpiDefaultOemId), sizeof (Header->OemId))
    );
  WriteUnaligned64 (&Header->OemTableId, PcdGet64 (PcdAcpiDefaultOemTableId));
  Header->OemRevision     = PcdGet32 (PcdAcpiDefaultOemRevision);
  Header->CreatorId       = PcdGet32 (PcdAcpiDefaultCreatorId);
  Header->CreatorRevision = PcdGet32 (PcdAcpiDefaultCreatorRevision);

  //
  // Install Boot Logo protocol.
  //
  Status = gBS->InstallMultipleProtocolInterfaces (
                  &mBootLogoHandle,
                  &gEfiBootLogoProtocolGuid,
                  &mBootLogoProtocolTemplate,
                  NULL
                  );
  ASSERT_EFI_ERROR (Status);

  //
  // Register notify function to install BGRT on ReadyToBoot Event.
  //
  Status = gBS->CreateEventEx (
                  EVT_NOTIFY_SIGNAL,
                  TPL_CALLBACK,
                  BgrtReadyToBootEventNotify,
                  NULL,
                  &gEfiEventReadyToBootGuid,
                  &mBootGraphicsReadyToBootEvent
                  );
  ASSERT_EFI_ERROR (Status);

  return Status;
}
