/*
* TLS Callbacks
* (C) 2016 Jack Lloyd
*     2017 Harry Reimann, Rohde & Schwarz Cybersecurity
*     2022 René Meusel, Hannes Rantzsch - neXenio GmbH
*     2023 René Meusel - Rohde & Schwarz Cybersecurity
*
* Botan is released under the Simplified BSD License (see license.txt)
*/

#include <botan/tls_callbacks.h>
#include <botan/tls_policy.h>
#include <botan/tls_algos.h>
#include <botan/x509path.h>
#include <botan/ocsp.h>
#include <botan/dh.h>
#include <botan/dl_group.h>
#include <botan/ecdh.h>
#include <botan/tls_exceptn.h>
#include <botan/internal/ct_utils.h>
#include <botan/internal/stl_util.h>

#if defined(BOTAN_HAS_CURVE_25519)
  #include <botan/curve25519.h>
#endif

namespace Botan {

void TLS::Callbacks::tls_inspect_handshake_msg(const Handshake_Message& /*unused*/)
   {
   // default is no op
   }

std::string TLS::Callbacks::tls_server_choose_app_protocol(const std::vector<std::string>& /*unused*/)
   {
   return "";
   }

std::string TLS::Callbacks::tls_peer_network_identity()
   {
   return "";
   }

std::chrono::system_clock::time_point TLS::Callbacks::tls_current_timestamp()
   {
   return std::chrono::system_clock::now();
   }

void TLS::Callbacks::tls_modify_extensions(Extensions& /*unused*/, Connection_Side /*unused*/, Handshake_Type /*unused*/)
   {
   }

void TLS::Callbacks::tls_examine_extensions(const Extensions& /*unused*/, Connection_Side /*unused*/, Handshake_Type /*unused*/)
   {
   }

bool TLS::Callbacks::tls_should_persist_resumption_information(const Session& session)
   {
   // RFC 5077 3.3
   //    The ticket_lifetime_hint field contains a hint from the server about
   //    how long the ticket should be stored. A value of zero is reserved to
   //    indicate that the lifetime of the ticket is unspecified.
   //
   // RFC 8446 4.6.1
   //    [A ticket_lifetime] of zero indicates that the ticket should be discarded
   //    immediately.
   //
   // By default we opt to keep all sessions, except for TLS 1.3 with a lifetime
   // hint of zero.
   return session.lifetime_hint().count() > 0 ||
          session.version().is_pre_tls_13();
   }

void TLS::Callbacks::tls_verify_cert_chain(
   const std::vector<X509_Certificate>& cert_chain,
   const std::vector<std::optional<OCSP::Response>>& ocsp_responses,
   const std::vector<Certificate_Store*>& trusted_roots,
   Usage_Type usage,
   const std::string& hostname,
   const TLS::Policy& policy)
   {
   if(cert_chain.empty())
      throw Invalid_Argument("Certificate chain was empty");

   Path_Validation_Restrictions restrictions(policy.require_cert_revocation_info(),
                                             policy.minimum_signature_strength());

   Path_Validation_Result result =
      x509_path_validate(cert_chain,
                         restrictions,
                         trusted_roots,
                         (usage == Usage_Type::TLS_SERVER_AUTH ? hostname : ""),
                         usage,
                         tls_current_timestamp(),
                         tls_verify_cert_chain_ocsp_timeout(),
                         ocsp_responses);

   if(!result.successful_validation())
      {
      throw TLS_Exception(Alert::BadCertificate,
                          "Certificate validation failure: " + result.result_string());
      }
   }

std::optional<OCSP::Response> TLS::Callbacks::tls_parse_ocsp_response(const std::vector<uint8_t>& raw_response)
   {
   try
      {
      return OCSP::Response(raw_response);
      }
   catch(const Decoding_Error&)
      {
      // ignore parsing errors and just ignore the broken OCSP response
      return std::nullopt;
      }
   }


std::vector<std::vector<uint8_t>> TLS::Callbacks::tls_provide_cert_chain_status(
   const std::vector<X509_Certificate>& chain,
   const Certificate_Status_Request& csr)
   {
   std::vector<std::vector<uint8_t>> result(chain.size());
   if(!chain.empty())
      {
      result[0] = tls_provide_cert_status(chain, csr);
      }
   return result;
   }

std::vector<uint8_t> TLS::Callbacks::tls_sign_message(
   const Private_Key& key,
   RandomNumberGenerator& rng,
   const std::string& emsa,
   Signature_Format format,
   const std::vector<uint8_t>& msg)
   {
   PK_Signer signer(key, rng, emsa, format);

   return signer.sign_message(msg, rng);
   }

bool TLS::Callbacks::tls_verify_message(
   const Public_Key& key,
   const std::string& emsa,
   Signature_Format format,
   const std::vector<uint8_t>& msg,
   const std::vector<uint8_t>& sig)
   {
   PK_Verifier verifier(key, emsa, format);

   return verifier.verify_message(msg, sig);
   }

namespace {

bool is_dh_group(const std::variant<TLS::Group_Params, DL_Group>& group)
   {
   return std::holds_alternative<DL_Group>(group) ||
          is_dh(std::get<TLS::Group_Params>(group));
   }

DL_Group get_dl_group(const std::variant<TLS::Group_Params, DL_Group>& group)
   {
   BOTAN_ASSERT_NOMSG(is_dh_group(group));

   // TLS 1.2 allows specifying arbitrary DL_Group parameters in-lieu of
   // a standardized DH group identifier. TLS 1.3 just offers pre-defined
   // groups.
   return std::visit(overloaded
      {
      [](const DL_Group& dl_group) { return dl_group; },
      [&](TLS::Group_Params group_param) { return DL_Group(group_param_to_string(group_param)); }
      }, group);
   }

}

std::unique_ptr<PK_Key_Agreement_Key> TLS::Callbacks::tls_generate_ephemeral_key(
   const std::variant<TLS::Group_Params, DL_Group>& group,
   RandomNumberGenerator& rng)
   {
   if(is_dh_group(group))
      {
      const DL_Group dl_group = get_dl_group(group);
      return std::make_unique<DH_PrivateKey>(rng, dl_group);
      }

   BOTAN_ASSERT_NOMSG(std::holds_alternative<TLS::Group_Params>(group));
   const auto group_params = std::get<TLS::Group_Params>(group);

   if(is_ecdh(group_params))
      {
      const EC_Group ec_group(group_param_to_string(group_params));
      return std::make_unique<ECDH_PrivateKey>(rng, ec_group);
      }

#if defined(BOTAN_HAS_CURVE_25519)
   if(is_x25519(group_params))
      {
      return std::make_unique<X25519_PrivateKey>(rng);
      }
#endif

   throw TLS_Exception(Alert::DecodeError, "cannot create a key offering without a group definition");
   }

secure_vector<uint8_t> TLS::Callbacks::tls_ephemeral_key_agreement(
   const std::variant<TLS::Group_Params, DL_Group>& group,
   const PK_Key_Agreement_Key& private_key,
   const std::vector<uint8_t>& public_value,
   RandomNumberGenerator& rng,
   const Policy& policy)
   {
   auto agree = [&](const PK_Key_Agreement_Key& sk, const auto& pk)
      {
      PK_Key_Agreement ka(sk, rng, "Raw");
      return ka.derive_key(0, pk.public_value()).bits_of();
      };

   if(is_dh_group(group))
      {
      // TLS 1.2 allows specifying arbitrary DL_Group parameters in-lieu of
      // a standardized DH group identifier.
      const auto dl_group = get_dl_group(group);

      auto Y = BigInt::decode(public_value);

      /*
       * A basic check for key validity. As we do not know q here we
       * cannot check that Y is in the right subgroup. However since
       * our key is ephemeral there does not seem to be any
       * advantage to bogus keys anyway.
       */
      if(Y <= 1 || Y >= dl_group.get_p() - 1)
         throw TLS_Exception(Alert::IllegalParameter,
                             "Server sent bad DH key for DHE exchange");

      DH_PublicKey peer_key(dl_group, Y);
      policy.check_peer_key_acceptable(peer_key);

      return agree(private_key, peer_key);
      }

   BOTAN_ASSERT_NOMSG(std::holds_alternative<TLS::Group_Params>(group));
   const auto group_params = std::get<TLS::Group_Params>(group);

   if(is_ecdh(group_params))
      {
      const EC_Group ec_group(group_param_to_string(group_params));
      ECDH_PublicKey peer_key(ec_group, ec_group.OS2ECP(public_value));
      policy.check_peer_key_acceptable(peer_key);

      return agree(private_key, peer_key);
      }

#if defined(BOTAN_HAS_CURVE_25519)
   if(is_x25519(group_params))
      {
      if(public_value.size() != 32)
         {
         throw TLS_Exception(Alert::HandshakeFailure, "Invalid X25519 key size");
         }

      Curve25519_PublicKey peer_key(public_value);
      policy.check_peer_key_acceptable(peer_key);

      return agree(private_key, peer_key);
      }
#endif

   throw TLS_Exception(Alert::IllegalParameter, "Did not recognize the key exchange group");
   }

}
