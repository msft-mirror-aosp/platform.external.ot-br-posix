/*
 *    Copyright (c) 2024, The OpenThread Authors.
 *    All rights reserved.
 *
 *    Redistribution and use in source and binary forms, with or without
 *    modification, are permitted provided that the following conditions are met:
 *    1. Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *    2. Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in the
 *       documentation and/or other materials provided with the distribution.
 *    3. Neither the name of the copyright holder nor the
 *       names of its contributors may be used to endorse or promote products
 *       derived from this software without specific prior written permission.
 *
 *    THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 *    AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 *    IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 *    ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 *    LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 *    CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 *    SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 *    INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 *    CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 *    ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 *    POSSIBILITY OF SUCH DAMAGE.
 */

package com.android.server.thread.openthread;

import com.android.server.thread.openthread.DnsTxtAttribute;

/**
 *  A collection of MeshCoP TXT entries that are supplied by Android platform.
 */
parcelable MeshcopTxtAttributes {
    /**
     * Predefined MeshCoP TXT entry named "mn".
     *
     * The length must not exceed 24 UTF-8 bytes.
     */
    String modelName;

    /**
     * Predefined MeshCoP TXT entry named "vn".
     *
     * The length must not exceed 24 UTF-8 bytes.
     */
    String vendorName;

    /**
     * Predefined MeshCoP TXT entry named "vo".
     *
     * The length must be 3 bytes.
     */
    byte[] vendorOui;

    /**
     * Non-standard (vendor-specific) _meshcop._udp TXT entries.
     *
     * All TXT keys MUST start with "v".
     *
     */
    List<DnsTxtAttribute> nonStandardTxtEntries;
}
