/*
 * Copyright (c) 2015, Cryptonomex, Inc.
 * All rights reserved.
 *
 * This source code is provided for evaluation in private test networks only, until September 8, 2015. After this date, this license expires and
 * the code may not be used, modified or distributed for any purpose. Redistribution and use in source and binary forms, with or without modification,
 * are permitted until September 8, 2015, provided that the following conditions are met:
 *
 * 1. The code and/or derivative works are used only for private test networks consisting of no more than 10 P2P nodes.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
 * THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF
 * ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */
#include <graphene/chain/exceptions.hpp>
#include <graphene/chain/protocol/fee_schedule.hpp>
#include <fc/io/raw.hpp>
#include <fc/bitutil.hpp>
#include <fc/smart_ref_impl.hpp>
#include <algorithm>

namespace graphene { namespace chain {


digest_type processed_transaction::merkle_digest()const
{
   return digest_type::hash(*this);
}

digest_type transaction::digest()const
{
   digest_type::encoder enc;
   fc::raw::pack( enc, *this );
   return enc.result();
}
void transaction::validate() const
{
   for( const auto& op : operations )
      operation_validate(op); 
}

graphene::chain::transaction_id_type graphene::chain::transaction::id() const
{
   digest_type::encoder enc;
   fc::raw::pack(enc, *this);
   auto hash = enc.result();
   transaction_id_type result;
   memcpy(result._hash, hash._hash, std::min(sizeof(result), sizeof(hash)));
   return result;
}

const signature_type& graphene::chain::signed_transaction::sign(const private_key_type& key)
{
   signatures.push_back(key.sign_compact(digest()));
   return signatures.back();
}
signature_type graphene::chain::signed_transaction::sign(const private_key_type& key)const
{
   return key.sign_compact(digest());
}

void transaction::set_expiration( fc::time_point_sec expiration_time )
{
    expiration = expiration_time;
}

void transaction::set_reference_block( const block_id_type& reference_block )
{
   ref_block_num = fc::endian_reverse_u32(reference_block._hash[0]);
   if( ref_block_num == 0 ) ref_block_prefix = 0;
   ref_block_prefix = reference_block._hash[1];
}

void transaction::get_required_authorities( flat_set<account_id_type>& active, flat_set<account_id_type>& owner, vector<authority>& other )const
{
   for( const auto& op : operations )
      operation_get_required_authorities( op, active, owner, other );
}




struct sign_state
{
      /** returns true if we have a signature for this key or can 
       * produce a signature for this key, else returns false. 
       */
      bool signed_by( const public_key_type& k )
      {
         auto itr = provided_signatures.find(k);
         if( itr == provided_signatures.end() )
         {
            auto pk = available_keys.find(k);
            if( pk  != available_keys.end() )
               return provided_signatures[k] = true;
            return false;
         }
         return itr->second = true;
      }

      bool check_authority( account_id_type id )
      {
         if( approved_by.find(id) != approved_by.end() ) return true;
         return check_authority( get_active(id) );
      }

      /**
       *  Checks to see if we have signatures of the active authorites of
       *  the accounts specified in authority or the keys specified. 
       */
      bool check_authority( const authority* au, uint32_t depth = 0 )
      {
         if( au == nullptr ) return false;
         const authority& auth = *au;

         uint32_t total_weight = 0;
         for( const auto& k : auth.key_auths )
            if( signed_by( k.first ) )
            {
               total_weight += k.second;
               if( total_weight >= auth.weight_threshold )
                  return true;
            }

         for( const auto& a : auth.account_auths )
         {
            if( approved_by.find(a.first) == approved_by.end() )
            {
               if( depth == max_recursion )
                  return false;
               if( check_authority( get_active( a.first ), depth+1 ) )
               {
                  approved_by.insert( a.first );
                  total_weight += a.second;
                  if( total_weight >= auth.weight_threshold )
                     return true;
               }
            }
            else
            {
               total_weight += a.second;
               if( total_weight >= auth.weight_threshold )
                  return true;
            }
         }
         return total_weight >= auth.weight_threshold;
      }

      bool remove_unused_signatures()
      {
         vector<public_key_type> remove_sigs;
         for( const auto& sig : provided_signatures )
            if( !sig.second ) remove_sigs.push_back( sig.first );

         for( auto& sig : remove_sigs )
            provided_signatures.erase(sig);

         return remove_sigs.size() != 0;
      }

      sign_state( const flat_set<public_key_type>& sigs,
                  const std::function<const authority*(account_id_type)>& a,
                  const flat_set<public_key_type>& keys = flat_set<public_key_type>() )
      :get_active(a),available_keys(keys)
      {
         for( const auto& key : sigs )
            provided_signatures[ key ] = false;
         approved_by.insert( GRAPHENE_TEMP_ACCOUNT  );
      }

      const std::function<const authority*(account_id_type)>& get_active;
      const flat_set<public_key_type>&                        available_keys;

      flat_map<public_key_type,bool>   provided_signatures;
      flat_set<account_id_type>        approved_by;
      uint32_t                         max_recursion = GRAPHENE_MAX_SIG_CHECK_DEPTH;
};


void verify_authority( const vector<operation>& ops, const flat_set<public_key_type>& sigs, 
                       const std::function<const authority*(account_id_type)>& get_active,
                       const std::function<const authority*(account_id_type)>& get_owner,
                       uint32_t max_recursion_depth,
                       bool  allow_committe,
                       const flat_set<account_id_type>& active_aprovals,
                       const flat_set<account_id_type>& owner_approvals )
{ try {
   flat_set<account_id_type> required_active;
   flat_set<account_id_type> required_owner;
   vector<authority> other;

   for( const auto& op : ops )
      operation_get_required_authorities( op, required_active, required_owner, other );

   if( !allow_committe )
      GRAPHENE_ASSERT( required_active.find(GRAPHENE_COMMITTEE_ACCOUNT) == required_active.end(),
                       invalid_committee_approval, "Committee account may only propose transactions" );

   sign_state s(sigs,get_active);
   s.max_recursion = max_recursion_depth;
   for( auto& id : active_aprovals )
      s.approved_by.insert( id );
   for( auto& id : owner_approvals )
      s.approved_by.insert( id );

   for( const auto& auth : other )
      GRAPHENE_ASSERT( s.check_authority(&auth), tx_missing_other_auth, "Missing Authority", ("auth",auth)("sigs",sigs) );

   // fetch all of the top level authorities
   for( auto id : required_active )
      GRAPHENE_ASSERT( s.check_authority(id) || 
                       s.check_authority(get_owner(id)), 
                       tx_missing_active_auth, "Missing Active Authority ${id}", ("id",id)("auth",*get_active(id))("owner",*get_owner(id)) );

   for( auto id : required_owner )
      GRAPHENE_ASSERT( owner_approvals.find(id) != owner_approvals.end() ||
                       s.check_authority(get_owner(id)), 
                       tx_missing_other_auth, "Missing Owner Authority ${id}", ("id",id)("auth",*get_owner(id)) );

   FC_ASSERT( !s.remove_unused_signatures(), "Unnecessary signatures detected" );
} FC_CAPTURE_AND_RETHROW( (ops)(sigs) ) }


flat_set<public_key_type> signed_transaction::get_signature_keys()const
{ try {
   auto d = digest();
   flat_set<public_key_type> result;
   for( const auto&  sig : signatures )
      FC_ASSERT( result.insert( fc::ecc::public_key(sig,d) ).second, "Duplicate Signature detected" );
   return result;
} FC_CAPTURE_AND_RETHROW() }



set<public_key_type> signed_transaction::get_required_signatures( const flat_set<public_key_type>& available_keys,
                                              const std::function<const authority*(account_id_type)>& get_active,
                                              const std::function<const authority*(account_id_type)>& get_owner,
                                              uint32_t max_recursion_depth )const
{
   flat_set<account_id_type> required_active;
   flat_set<account_id_type> required_owner;
   vector<authority> other;
   get_required_authorities( required_active, required_owner, other );


   sign_state s(get_signature_keys(),get_active,available_keys);
   s.max_recursion = max_recursion_depth;

   for( const auto& auth : other )
      s.check_authority(&auth);
   for( auto& owner : required_owner )
      s.check_authority( get_owner( owner ) );
   for( auto& active : required_active )
      s.check_authority( active  );

   s.remove_unused_signatures();

   set<public_key_type> result;

   for( auto& provided_sig : s.provided_signatures )
      if( available_keys.find( provided_sig.first ) != available_keys.end() )
         result.insert( provided_sig.first );

   return result;
}

set<public_key_type> signed_transaction::minimize_required_signatures(
   const flat_set<public_key_type>& available_keys,
   const std::function<const authority*(account_id_type)>& get_active,
   const std::function<const authority*(account_id_type)>& get_owner,
   uint32_t max_recursion
   ) const
{
   set< public_key_type > s = get_required_signatures( available_keys, get_active, get_owner, max_recursion );
   flat_set< public_key_type > result( s.begin(), s.end() );

   for( const public_key_type& k : s )
   {
      result.erase( k );
      try
      {
         graphene::chain::verify_authority( operations, result, get_active, get_owner, max_recursion );
         continue;  // element stays erased if verify_authority is ok
      }
      catch( const tx_missing_owner_auth& e ) {}
      catch( const tx_missing_active_auth& e ) {}
      catch( const tx_missing_other_auth& e ) {}
      result.insert( k );
   }
   return set<public_key_type>( result.begin(), result.end() );
}

void signed_transaction::verify_authority( const std::function<const authority*(account_id_type)>& get_active,
                                           const std::function<const authority*(account_id_type)>& get_owner,
                                           uint32_t max_recursion )const
{ try {
   graphene::chain::verify_authority( operations, get_signature_keys(), get_active, get_owner, max_recursion );
} FC_CAPTURE_AND_RETHROW( (*this) ) }

void transaction::get_impacted_accounts( flat_set<account_id_type>& impacted ) const
{
   for( const auto& op : operations )
      operation_get_impacted_accounts( op, impacted );
}

} } // graphene::chain
