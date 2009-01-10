/*
 *  Licensed to the Apache Software Foundation (ASF) under one or more
 *  contributor license agreements.  See the NOTICE file distributed with
 *  this work for additional information regarding copyright ownership.
 *  The ASF licenses this file to You under the Apache License, Version 2.0
 *  (the "License"); you may not use this file except in compliance with
 *  the License.  You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 */

package tests.security.cert;

import dalvik.annotation.TestTargets;
import dalvik.annotation.TestLevel;
import dalvik.annotation.TestTargetNew;
import dalvik.annotation.TestTargetClass;

import junit.framework.TestCase;

import java.security.cert.PolicyNode;
import java.security.cert.PolicyQualifierInfo;
import java.util.HashSet;
import java.util.Set;
import java.util.Iterator;

import org.apache.harmony.security.tests.support.cert.PolicyNodeImpl;

/**
 * Tests for <code>java.security.cert.PolicyNode</code> fields and methods
 * 
 */
@TestTargetClass(PolicyNode.class)
public class PolicyNodeTest extends TestCase {
    
    private String validPolicy = "ValidPolicy";
    private String anyPolicy = "2.5.29.32.0";
    private boolean criticalityIndicator = true;
    private HashSet hs = null;
    
    /**
     * Returns valid DER encoding for the following ASN.1 definition
     * (as specified in RFC 3280 -
     *  Internet X.509 Public Key Infrastructure.
     *  Certificate and Certificate Revocation List (CRL) Profile.
     *  http://www.ietf.org/rfc/rfc3280.txt):
     * 
     *   PolicyQualifierInfo ::= SEQUENCE {
     *      policyQualifierId       PolicyQualifierId,
     *      qualifier               ANY DEFINED BY policyQualifierId
     *   }
     * 
     * where policyQualifierId (OID) is
     *      1.3.6.1.5.5.7.2.1
     * and qualifier (IA5String) is
     *      "http://www.qq.com/stmt.txt"
     *      
     * (data generated by own encoder during test development)
     */
    private static final byte[] getDerEncoding() {
        // DO NOT MODIFY!
        return  new byte[] {
            (byte)0x30, (byte)0x26, // tag Seq, length
              (byte)0x06, (byte)0x08, // tag OID, length
                (byte)0x2b, (byte)0x06, (byte)0x01, (byte)0x05, // oid value 
                (byte)0x05, (byte)0x07, (byte)0x02, (byte)0x01, // oid value
              (byte)0x16, (byte)0x1a, // tag IA5String, length
                (byte)0x68, (byte)0x74, (byte)0x74, (byte)0x70,  // IA5String value
                (byte)0x3a, (byte)0x2f, (byte)0x2f, (byte)0x77,  // IA5String value
                (byte)0x77, (byte)0x77, (byte)0x2e, (byte)0x71,  // IA5String value
                (byte)0x71, (byte)0x2e, (byte)0x63, (byte)0x6f,  // IA5String value
                (byte)0x6d, (byte)0x2f, (byte)0x73, (byte)0x74,  // IA5String value
                (byte)0x6d, (byte)0x74, (byte)0x2e, (byte)0x74,  // IA5String value
                (byte)0x78, (byte)0x74   // IA5String value
        };
    }
    
    protected void setUp() {
        hs = new HashSet();
        hs.add(new String("StringParameter1"));
        hs.add(new String("StringParameter2"));
        hs.add(new String("StringParameter3"));
    }
    
    protected void setUp1() {
        hs = new HashSet();
        try {
            hs.add(new PolicyQualifierInfo(getDerEncoding()));
        } catch (Exception e) {
            fail("Ezxception " + e + " for setUp1()");
        }
    }


    /**
     * Constructor for CRLTest.
     * @param name
     */
    public PolicyNodeTest(String name) {
        super(name);
    }
    
    class MyPolicyNode extends PolicyNodeImpl {
        MyPolicyNode(PolicyNodeImpl policynode, String s, Set set, 
                     boolean flag, Set set1, boolean flag1) {
            super(policynode, s, set, flag, set1, flag1);
        }
    }

    //
    // Tests
    //

    /**
     * @tests java.security.cert.PolicyNode#getDepth() 
     */
    @TestTargetNew(
        level = TestLevel.COMPLETE,
        notes = "",
        method = "getDepth",
        args = {}
    )
    public final void test_getDepth() {
        MyPolicyNode pn = new MyPolicyNode(null, validPolicy, null, criticalityIndicator, null, true);
        try {
            assertEquals(pn.getDepth(), 0);
        } catch (Exception e) {
            fail("Unexpected exception: " + e);
        }
        MyPolicyNode pn1 = new MyPolicyNode(pn, validPolicy, null, criticalityIndicator, null, true);
        try {
            assertEquals(pn1.getDepth(), 1);
        } catch (Exception e) {
            fail("Unexpected exception: " + e);
        }
    }
    
    /**
     * @tests java.security.cert.PolicyNode#getValidPolicy() 
     */
    @TestTargetNew(
        level = TestLevel.COMPLETE,
        notes = "",
        method = "getValidPolicy",
        args = {}
    )
    public final void test_getValidPolicy() {
        MyPolicyNode pn = new MyPolicyNode(null, null, null, criticalityIndicator, null, true);
        try {
            assertEquals(pn.getValidPolicy(), "");
        } catch (Exception e) {
            fail("Unexpected exception: " + e);
        }
        pn = new MyPolicyNode(pn, validPolicy, null, criticalityIndicator, null, true);
        try {
            assertEquals(pn.getValidPolicy(), "ValidPolicy");
        } catch (Exception e) {
            fail("Unexpected exception: " + e);
        }
        pn = new MyPolicyNode(pn, anyPolicy, null, criticalityIndicator, null, true);
        try {
            assertEquals(pn.getValidPolicy(), "2.5.29.32.0");
        } catch (Exception e) {
            fail("Unexpected exception: " + e);
        }
    }
    
    /**
     * @tests java.security.cert.PolicyNode#isCritical() 
     */
    @TestTargetNew(
        level = TestLevel.COMPLETE,
        notes = "",
        method = "isCritical",
        args = {}
    )
    public final void test_isCritical() {
        MyPolicyNode pn = new MyPolicyNode(null, anyPolicy, null, criticalityIndicator, null, true);
        try {
            assertEquals(pn.isCritical(), true);
        } catch (Exception e) {
            fail("Unexpected exception: " + e);
        }
        criticalityIndicator = false;
        pn = new MyPolicyNode(null, validPolicy, null, criticalityIndicator, null, true);
        try {
            assertEquals(pn.isCritical(), false);
        } catch (Exception e) {
            fail("Unexpected exception: " + e);
        }
    }
    
    /**
     * @tests java.security.cert.PolicyNode#getParent() 
     */
    @TestTargetNew(
        level = TestLevel.COMPLETE,
        notes = "",
        method = "getParent",
        args = {}
    )
    public final void test_getParent() {
        MyPolicyNode pn = new MyPolicyNode(null, anyPolicy, null, criticalityIndicator, null, true);
        try {
            assertNull(pn.getParent());
            assertEquals(pn.getDepth(), 0);
        } catch (Exception e) {
            fail("Unexpected exception: " + e);
        }
        MyPolicyNode pn1 = new MyPolicyNode(pn, anyPolicy, null, criticalityIndicator, null, true);
        try {
            PolicyNode newPN = pn1.getParent();
            assertEquals(newPN.getDepth(), 0);
        } catch (Exception e) {
            fail("Unexpected exception: " + e);
        }
        MyPolicyNode pn2 = new MyPolicyNode(pn1, anyPolicy, null, criticalityIndicator, null, true);
        try {
            PolicyNode newPN = pn2.getParent();
            assertEquals(newPN.getDepth(), 1);
        } catch (Exception e) {
            fail("Unexpected exception: " + e);
        }
    }
    
    /**
     * @tests java.security.cert.PolicyNode#getExpectedPolicies() 
     */
    @TestTargetNew(
        level = TestLevel.COMPLETE,
        notes = "",
        method = "getExpectedPolicies",
        args = {}
    )
    public final void test_getExpectedPolicies() {
        setUp();
        MyPolicyNode pn = new MyPolicyNode(null, anyPolicy, null, criticalityIndicator, hs, true);
        try {
            Set res = pn.getExpectedPolicies();
            assertEquals(res.size(), hs.size());
            assertEquals(res, hs);
        } catch (Exception e) {
            fail("Unexpected exception: " + e);
        }
    }
    
    /**
     * @tests java.security.cert.PolicyNode#getPolicyQualifiers() 
     */
    @TestTargetNew(
        level = TestLevel.COMPLETE,
        notes = "",
        method = "getPolicyQualifiers",
        args = {}
    )
    public final void test_getPolicyQualifiers() {
        setUp1();
        MyPolicyNode pn = new MyPolicyNode(null, anyPolicy, hs, criticalityIndicator, null, true);
        try {
            Set res = pn.getPolicyQualifiers();
            assertEquals(res.size(), hs.size());
            assertEquals(res, hs);
        } catch (Exception e) {
            fail("Unexpected exception: " + e);
        }
    }
    
    /**
     * @tests java.security.cert.PolicyNode#getChildren() 
     */
    @TestTargetNew(
        level = TestLevel.COMPLETE,
        notes = "",
        method = "getChildren",
        args = {}
    )
    public final void test_getChildren() {
        MyPolicyNode pn = new MyPolicyNode(null, anyPolicy, null, criticalityIndicator, null, true);
        Iterator it = pn.getChildren();
        try {
            it.remove();
            fail("UnsupportedOperationException was not thrown");
        } catch (UnsupportedOperationException uoe) {
            //expected
        }
        MyPolicyNode pn1 = new MyPolicyNode(pn, anyPolicy, null, criticalityIndicator, null, true);
        try {
            it = pn1.getChildren();
            assertFalse(it.hasNext());
        } catch (Exception e) {
            fail("Unexpected exception: " + e);
        }
    }
}