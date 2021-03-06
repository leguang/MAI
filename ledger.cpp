#include <mol/blockstore.hpp>
#include <mol/ledger.hpp>
#include <mol/node/common.hpp>
#include <mol/node/stats.hpp>

namespace
{
/**
 * Roll back the visited block
 */
class rollback_visitor : public mol::block_visitor
{
public:
	rollback_visitor (MDB_txn * transaction_a, mol::ledger & ledger_a) :
	transaction (transaction_a),
	ledger (ledger_a)
	{
	}
	virtual ~rollback_visitor () = default;
	void send_block (mol::send_block const & block_a) override
	{
		auto hash (block_a.hash ());
		mol::pending_info pending;
		mol::pending_key key (block_a.hashables.destination, hash);
		while (ledger.store.pending_get (transaction, key, pending))
		{
			ledger.rollback (transaction, ledger.latest (transaction, block_a.hashables.destination));
		}
		mol::account_info info;
		auto error (ledger.store.account_get (transaction, pending.source, info));
		assert (!error);
		ledger.store.pending_del (transaction, key);
		ledger.store.representation_add (transaction, ledger.representative (transaction, hash), pending.amount.number ());
		ledger.change_latest (transaction, pending.source, block_a.hashables.previous, info.rep_block, ledger.balance (transaction, block_a.hashables.previous), info.block_count - 1);
		ledger.store.block_del (transaction, hash);
		ledger.store.frontier_del (transaction, hash);
		ledger.store.frontier_put (transaction, block_a.hashables.previous, pending.source);
		ledger.store.block_successor_clear (transaction, block_a.hashables.previous);
		if (!(info.block_count % ledger.store.block_info_max))
		{
			ledger.store.block_info_del (transaction, hash);
		}
		ledger.stats.inc (mol::stat::type::rollback, mol::stat::detail::send);
	}
	void receive_block (mol::receive_block const & block_a) override
	{
		auto hash (block_a.hash ());
		auto representative (ledger.representative (transaction, block_a.hashables.previous));
		auto amount (ledger.amount (transaction, block_a.hashables.source));
		auto destination_account (ledger.account (transaction, hash));
		auto source_account (ledger.account (transaction, block_a.hashables.source));
		mol::account_info info;
		auto error (ledger.store.account_get (transaction, destination_account, info));
		assert (!error);
		ledger.store.representation_add (transaction, ledger.representative (transaction, hash), 0 - amount);
		ledger.change_latest (transaction, destination_account, block_a.hashables.previous, representative, ledger.balance (transaction, block_a.hashables.previous), info.block_count - 1);
		ledger.store.block_del (transaction, hash);
		ledger.store.pending_put (transaction, mol::pending_key (destination_account, block_a.hashables.source), { source_account, amount });
		ledger.store.frontier_del (transaction, hash);
		ledger.store.frontier_put (transaction, block_a.hashables.previous, destination_account);
		ledger.store.block_successor_clear (transaction, block_a.hashables.previous);
		if (!(info.block_count % ledger.store.block_info_max))
		{
			ledger.store.block_info_del (transaction, hash);
		}
		ledger.stats.inc (mol::stat::type::rollback, mol::stat::detail::receive);
	}
	void open_block (mol::open_block const & block_a) override
	{
		auto hash (block_a.hash ());
		auto amount (ledger.amount (transaction, block_a.hashables.source));
		auto destination_account (ledger.account (transaction, hash));
		auto source_account (ledger.account (transaction, block_a.hashables.source));
		ledger.store.representation_add (transaction, ledger.representative (transaction, hash), 0 - amount);
		ledger.change_latest (transaction, destination_account, 0, 0, 0, 0);
		ledger.store.block_del (transaction, hash);
		ledger.store.pending_put (transaction, mol::pending_key (destination_account, block_a.hashables.source), { source_account, amount });
		ledger.store.frontier_del (transaction, hash);
		ledger.stats.inc (mol::stat::type::rollback, mol::stat::detail::open);
	}
	void change_block (mol::change_block const & block_a) override
	{
		auto hash (block_a.hash ());
		auto representative (ledger.representative (transaction, block_a.hashables.previous));
		auto account (ledger.account (transaction, block_a.hashables.previous));
		mol::account_info info;
		auto error (ledger.store.account_get (transaction, account, info));
		assert (!error);
		auto balance (ledger.balance (transaction, block_a.hashables.previous));
		ledger.store.representation_add (transaction, representative, balance);
		ledger.store.representation_add (transaction, hash, 0 - balance);
		ledger.store.block_del (transaction, hash);
		ledger.change_latest (transaction, account, block_a.hashables.previous, representative, info.balance, info.block_count - 1);
		ledger.store.frontier_del (transaction, hash);
		ledger.store.frontier_put (transaction, block_a.hashables.previous, account);
		ledger.store.block_successor_clear (transaction, block_a.hashables.previous);
		if (!(info.block_count % ledger.store.block_info_max))
		{
			ledger.store.block_info_del (transaction, hash);
		}
		ledger.stats.inc (mol::stat::type::rollback, mol::stat::detail::change);
	}
	void state_block (mol::state_block const & block_a) override
	{
		auto hash (block_a.hash ());
		mol::block_hash representative (0);
		if (!block_a.hashables.previous.is_zero ())
		{
			representative = ledger.representative (transaction, block_a.hashables.previous);
		}
		auto balance (ledger.balance (transaction, block_a.hashables.previous));
		auto is_send (block_a.hashables.balance < balance);
		// Add in amount delta
		ledger.store.representation_add (transaction, hash, 0 - block_a.hashables.balance.number ());
		if (!representative.is_zero ())
		{
			// Move existing representation
			ledger.store.representation_add (transaction, representative, balance);
		}

		if (is_send)
		{
			mol::pending_key key (block_a.hashables.link, hash);
			while (!ledger.store.pending_exists (transaction, key))
			{
				ledger.rollback (transaction, ledger.latest (transaction, block_a.hashables.link));
			}
			ledger.store.pending_del (transaction, key);
			ledger.stats.inc (mol::stat::type::rollback, mol::stat::detail::send);
		}
		else if (!block_a.hashables.link.is_zero ())
		{
			mol::pending_info info (ledger.account (transaction, block_a.hashables.link), block_a.hashables.balance.number () - balance);
			ledger.store.pending_put (transaction, mol::pending_key (block_a.hashables.account, block_a.hashables.link), info);
			ledger.stats.inc (mol::stat::type::rollback, mol::stat::detail::receive);
		}

		mol::account_info info;
		auto error (ledger.store.account_get (transaction, block_a.hashables.account, info));
		assert (!error);
		ledger.change_latest (transaction, block_a.hashables.account, block_a.hashables.previous, representative, balance, info.block_count - 1);

		auto previous (ledger.store.block_get (transaction, block_a.hashables.previous));
		if (previous != nullptr)
		{
			ledger.store.block_successor_clear (transaction, block_a.hashables.previous);
			if (previous->type () < mol::block_type::state)
			{
				ledger.store.frontier_put (transaction, block_a.hashables.previous, block_a.hashables.account);
			}
		}
		else
		{
			ledger.stats.inc (mol::stat::type::rollback, mol::stat::detail::open);
		}
		ledger.store.block_del (transaction, hash);
	}
	MDB_txn * transaction;
	mol::ledger & ledger;
};

class ledger_processor : public mol::block_visitor
{
public:
	ledger_processor (mol::ledger &, MDB_txn *);
	virtual ~ledger_processor () = default;
	void send_block (mol::send_block const &) override;
	void receive_block (mol::receive_block const &) override;
	void open_block (mol::open_block const &) override;
	void change_block (mol::change_block const &) override;
	void astate_block (mol::astate_block const &) override;
	void state_block (mol::state_block const &) override;
	void state_block_impl (mol::state_block const &);
	mol::ledger & ledger;
	MDB_txn * transaction;
	mol::process_return result;
};

void ledger_processor::state_block (mol::state_block const & block_a)
{
	result.code = ledger.state_block_parsing_enabled (transaction) ? mol::process_result::progress : mol::process_result::state_block_disabled;
	if (result.code == mol::process_result::progress)
	{
		state_block_impl (block_a);
	}
}

void ledger_processor::state_block_impl (mol::state_block const & block_a)
{
	auto hash (block_a.hash ());
	auto existing (ledger.store.block_exists (transaction, hash));
	result.code = existing ? mol::process_result::old : mol::process_result::progress; // Have we seen this block before? (Unambiguous)
	if (result.code == mol::process_result::progress)
	{
		result.code = validate_message (block_a.hashables.account, hash, block_a.signature) ? mol::process_result::bad_signature : mol::process_result::progress; // Is this block signed correctly (Unambiguous)
		if (result.code == mol::process_result::progress)
		{
			result.code = block_a.hashables.account.is_zero () ? mol::process_result::opened_burn_account : mol::process_result::progress; // Is this for the burn account? (Unambiguous)
			if (result.code == mol::process_result::progress)
			{
				mol::account_info info;
				result.amount = block_a.hashables.balance;
				auto is_send (false);
				auto account_error (ledger.store.account_get (transaction, block_a.hashables.account, info));
				if (!account_error)
				{
					// Account already exists
					result.code = block_a.hashables.previous.is_zero () ? mol::process_result::fork : mol::process_result::progress; // Has this account already been opened? (Ambigious)
					if (result.code == mol::process_result::progress)
					{
						result.code = ledger.store.block_exists (transaction, block_a.hashables.previous) ? mol::process_result::progress : mol::process_result::gap_previous; // Does the previous block exist in the ledger? (Unambigious)
						if (result.code == mol::process_result::progress)
						{
							is_send = block_a.hashables.balance < info.balance;
							result.amount = is_send ? (info.balance.number () - result.amount.number ()) : (result.amount.number () - info.balance.number ());
							result.code = block_a.hashables.previous == info.head ? mol::process_result::progress : mol::process_result::fork; // Is the previous block the account's head block? (Ambigious)
						}
					}
				}
				else
				{
					// Account does not yet exists
					result.code = block_a.previous ().is_zero () ? mol::process_result::progress : mol::process_result::gap_previous; // Does the first block in an account yield 0 for previous() ? (Unambigious)
					if (result.code == mol::process_result::progress)
					{
						ledger.stats.inc (mol::stat::type::ledger, mol::stat::detail::open);
						result.code = !block_a.hashables.link.is_zero () ? mol::process_result::progress : mol::process_result::gap_source; // Is the first block receiving from a send ? (Unambigious)
					}
				}
				if (result.code == mol::process_result::progress)
				{
					if (!is_send)
					{
						if (!block_a.hashables.link.is_zero ())
						{
							result.code = ledger.store.block_exists (transaction, block_a.hashables.link) ? mol::process_result::progress : mol::process_result::gap_source; // Have we seen the source block already? (Harmless)
							if (result.code == mol::process_result::progress)
							{
								mol::pending_key key (block_a.hashables.account, block_a.hashables.link);
								mol::pending_info pending;
								result.code = ledger.store.pending_get (transaction, key, pending) ? mol::process_result::unreceivable : mol::process_result::progress; // Has this source already been received (Malformed)
								if (result.code == mol::process_result::progress)
								{
									result.code = result.amount == pending.amount ? mol::process_result::progress : mol::process_result::balance_mismatch;
								}
							}
						}
						else
						{
							// If there's no link, the balance must remain the same, only the representative can change
							result.code = result.amount.is_zero () ? mol::process_result::progress : mol::process_result::balance_mismatch;
						}
					}
				}
				if (result.code == mol::process_result::progress)
				{
					ledger.stats.inc (mol::stat::type::ledger, mol::stat::detail::state_block);
					result.state_is_send = is_send;
					ledger.store.block_put (transaction, hash, block_a);

					if (!info.rep_block.is_zero ())
					{
						// Move existing representation
						ledger.store.representation_add (transaction, info.rep_block, 0 - info.balance.number ());
					}
					// Add in amount delta
					ledger.store.representation_add (transaction, hash, block_a.hashables.balance.number ());

					if (is_send)
					{
						mol::pending_key key (block_a.hashables.link, hash);
						mol::pending_info info (block_a.hashables.account, result.amount.number ());
						ledger.store.pending_put (transaction, key, info);
						ledger.stats.inc (mol::stat::type::ledger, mol::stat::detail::send);
					}
					else if (!block_a.hashables.link.is_zero ())
					{
						ledger.store.pending_del (transaction, mol::pending_key (block_a.hashables.account, block_a.hashables.link));
						ledger.stats.inc (mol::stat::type::ledger, mol::stat::detail::receive);
					}

					ledger.change_latest (transaction, block_a.hashables.account, hash, hash, block_a.hashables.balance, info.block_count + 1, true);
					if (!ledger.store.frontier_get (transaction, info.head).is_zero ())
					{
						ledger.store.frontier_del (transaction, info.head);
					}
					// Frontier table is unnecessary for state blocks and this also prevents old blocks from being inserted on top of state blocks
					result.account = block_a.hashables.account;
				}
			}
		}
	}
}

//added by sandy - s
void ledger_processor::astate_block (mol::astate_block const & block_a) {

	auto hash (block_a.hash ());
	//ledger里是否存在block_a
	auto existing (ledger.store.block_exists (transaction, hash));
	result.code = existing ? mol::process_result::old : mol::process_result::progress; // Have we seen this block before? (Unambiguous)
	if (result.code == mol::process_result::progress) {

		//validate signature
		result.code = validate_message (block_a.hashables.account, hash, block_a.signature) ? mol::process_result::bad_signature : mol::process_result::progress; // Is this block signed correctly (Unambiguous)
		if (result.code == mol::process_result::progress) {

			//是否是销毁account
			result.code = block_a.hashables.account.is_zero () ? mol::process_result::opened_burn_account : mol::process_result::progress; // Is this for the burn account? (Unambiguous)
			if (result.code == mol::process_result::progress) {

				//ledger里是否存在account
				auto existing (ledger.store.account_exists(transaction, block_a.hashables.account));
				result.code = existing ? mol::process_result::progress : mol::process_result::account_not_exist;
				if (result.code == mol::process_result::progress) {

					//获取ledger account_info
					mol::account_info info;
					result.amount = block_a.hashables.balance;
					auto is_send (false);
					auto account_error (ledger.store.account_get (transaction, block_a.hashables.account, info));
					if (!account_error) {

						//ledger里是否存在这个asset
						auto existing (ledger.store.asset_exists(transaction, block_a.hashables.asset));
						result.code = existing ? mol::process_result::progress : mol::process_result::asset_not_exist;
						if (result.code == mol::process_result::progress) {

							//account对应的asset是否存在
							auto existing (ledger.store.asset_account_exists(transaction, mol::asset_account_key(block_a.hashables.account, block_a.hashables.asset)));
							result.code = existing ? mol::process_result::progress : mol::process_result::account_asset_not_exist;
							if (result.code == mol::process_result::progress) {

								// astete send / receive block

								// 如果block_a previous为zero, block_a错误
								result.code = block_a.hashables.previous.is_zero () ? mol::process_result::block_previous_error : mol::process_result::progress; // Has this account already been opened? (Ambigious)
								if (result.code == mol::process_result::progress) {

									//如果block_a previous不存在,  则设置为mol::process_result::gap_previous
									result.code = ledger.store.block_exists(transaction, block_a.hashables.previous)
												  ? mol::process_result::progress
												  : mol::process_result::gap_previous; // Does the previous block exist in the ledger? (Unambigious)
									if (result.code == mol::process_result::progress) {

										//mol::block_hash asset_account_head = ledger.asset_account_latest(transaction, block_a.hashables.account, block_a.hashables.asset);
										//block previous 是否是 head block
										//result.code = block_a.hashables.previous == asset_account_head ? mol::process_result::progress : mol::process_result::block_previous_error;

										mol::asset_account_info info_asset_account;
										auto latest_error(ledger.store.asset_account_get(transaction,
																						 mol::asset_account_key(
																								 block_a.hashables.account,
																								 block_a.hashables.asset),
																						 info_asset_account));
										if (!latest_error) {

											//block previous 是否是 head block
											result.code = block_a.hashables.previous == info_asset_account.head ? mol::process_result::progress : mol::process_result::block_previous_error;
											if (result.code == mol::process_result::progress) {

												//如果新block余额 < 存在余额info.balance, 就是send block
												is_send = block_a.hashables.balance < info_asset_account.balance;
												result.amount = is_send ? (info_asset_account.balance.number () - result.amount.number ()) : (result.amount.number () - info_asset_account.balance.number ());

												if (is_send) { // send block

													//把block加到store
													ledger.store.block_put (transaction, hash, block_a);
													ledger.store.asset_account_put(transaction, mol::asset_account_key(block_a.hashables.account, block_a.hashables.asset), mol::asset_account_info(hash, info_asset_account.rep_block, info_asset_account.open_block, block_a.hashables.balance, mol::seconds_since_epoch (), info_asset_account.block_count + 1));

													//如果是发送块, 设置pending表, 等待接收块
													mol::pending_key key (block_a.hashables.link, hash);
													mol::pending_info info (block_a.hashables.account, result.amount.number ());
													ledger.store.pending_put (transaction, key, info);

												} else if (!block_a.hashables.link.is_zero ()) { // receive block

													//如果link对应的block不存在, 则mol::process_result::gap_source
													result.code = ledger.store.block_exists (transaction, block_a.hashables.link) ? mol::process_result::progress : mol::process_result::gap_source; // Have we seen the source block already? (Harmless)
													if (result.code == mol::process_result::progress) {

														mol::pending_key key (block_a.hashables.account, block_a.hashables.link);
														mol::pending_info pending;
														//如果在pending表找不到pending_info, 则mol::process_result::unreceivable
														result.code = ledger.store.pending_get (transaction, key, pending) ? mol::process_result::unreceivable : mol::process_result::progress; // Has this source already been received (Malformed)
														if (result.code == mol::process_result::progress) {

															//如果接收费用不一致, 则mol::process_result::balance_mismatch
															result.code = result.amount == pending.amount ? mol::process_result::progress : mol::process_result::balance_mismatch;

															if (result.code == mol::process_result::progress) {

																//把block加到store
																ledger.store.block_put (transaction, hash, block_a);
																ledger.store.asset_account_put(transaction, mol::asset_account_key(block_a.hashables.account, block_a.hashables.asset), mol::asset_account_info(hash, info_asset_account.rep_block, info_asset_account.open_block, block_a.hashables.balance, mol::seconds_since_epoch (), info_asset_account.block_count + 1));

																//删除pending表的对应数据
																ledger.store.pending_del (transaction, mol::pending_key (block_a.hashables.account, block_a.hashables.link));
															}

														}

													}


												}

											}
										}

									}

								}



							} else if (result.code == mol::process_result::account_asset_not_exist) {

								// astate open block

								// 如果block_a previous为zero, block_a错误
								result.code = block_a.hashables.previous.is_zero () ? mol::process_result::block_previous_error : mol::process_result::progress; // Has this account already been opened? (Ambigious)
								if (result.code == mol::process_result::progress) {

									//如果block_a previous不存在,  则设置为mol::process_result::gap_previous
									result.code = ledger.store.block_exists(transaction, block_a.hashables.previous)
												  ? mol::process_result::progress
												  : mol::process_result::gap_previous; // Does the previous block exist in the ledger? (Unambigious)
									if (result.code == mol::process_result::progress) {

										//block previous 是否是 开头block
										result.code = block_a.hashables.previous == info.open_block ? mol::process_result::progress : mol::process_result::block_previous_error;

										if (result.code == mol::process_result::progress) {

											//block link 是否是 zero
											if (!block_a.hashables.link.is_zero ()) {

												//如果link对应的block不存在, 则mol::process_result::gap_source
												result.code = ledger.store.block_exists (transaction, block_a.hashables.link) ? mol::process_result::progress : mol::process_result::gap_source; // Have we seen the source block already? (Harmless)
												if (result.code == mol::process_result::progress) {

													mol::pending_key key (block_a.hashables.account, block_a.hashables.link);
													mol::pending_info pending;
													//如果在pending表找不到pending_info, 则mol::process_result::unreceivable
													result.code = ledger.store.pending_get (transaction, key, pending) ? mol::process_result::unreceivable : mol::process_result::progress; // Has this source already been received (Malformed)
													if (result.code == mol::process_result::progress) {

														//如果接收费用不一致, 则mol::process_result::balance_mismatch
														result.code = result.amount == pending.amount ? mol::process_result::progress : mol::process_result::balance_mismatch;

														if (result.code == mol::process_result::progress) {

															//把block加到store
															ledger.store.block_put (transaction, hash, block_a);
															ledger.store.asset_account_put(transaction, mol::asset_account_key(block_a.hashables.account, block_a.hashables.asset), mol::asset_account_info(hash, info.rep_block, hash, block_a.hashables.balance, mol::seconds_since_epoch (), 1));

															//删除pending表的对应数据
															ledger.store.pending_del (transaction, mol::pending_key (block_a.hashables.account, block_a.hashables.link));

														}

													}

												}

											}

										}

									}

								}

							}


						} else if (result.code == mol::process_result::asset_not_exist) {

							//创建一个新asset

							// 如果block_a previous为zero, block_a错误
							result.code = block_a.hashables.previous.is_zero () ? mol::process_result::block_previous_error : mol::process_result::progress; // Has this account already been opened? (Ambigious)
							if (result.code == mol::process_result::progress) {

								//如果block_a previous不存在,  则设置为mol::process_result::gap_previous
								result.code = ledger.store.block_exists(transaction, block_a.hashables.previous)
											  ? mol::process_result::progress
											  : mol::process_result::gap_previous; // Does the previous block exist in the ledger? (Unambigious)
								if (result.code == mol::process_result::progress) {

									//把block加到store
									ledger.store.block_put (transaction, hash, block_a);
									ledger.store.asset_put(transaction, block_a.hashables.asset, block_a.hashables.account);
									ledger.store.asset_account_put(transaction, mol::asset_account_key(block_a.hashables.account, block_a.hashables.asset), mol::asset_account_info(hash, info.rep_block, hash, block_a.hashables.balance, mol::seconds_since_epoch (), 1));

								}

							}

						}

					}

				}


			}

		}

	}

}
//added by sandy - e

void ledger_processor::change_block (mol::change_block const & block_a)
{
	auto hash (block_a.hash ());
	auto existing (ledger.store.block_exists (transaction, hash));
	result.code = existing ? mol::process_result::old : mol::process_result::progress; // Have we seen this block before? (Harmless)
	if (result.code == mol::process_result::progress)
	{
		auto previous (ledger.store.block_get (transaction, block_a.hashables.previous));
		result.code = previous != nullptr ? mol::process_result::progress : mol::process_result::gap_previous; // Have we seen the previous block already? (Harmless)
		if (result.code == mol::process_result::progress)
		{
			result.code = block_a.valid_predecessor (*previous) ? mol::process_result::progress : mol::process_result::block_position;
			if (result.code == mol::process_result::progress)
			{
				auto account (ledger.store.frontier_get (transaction, block_a.hashables.previous));
				result.code = account.is_zero () ? mol::process_result::fork : mol::process_result::progress;
				if (result.code == mol::process_result::progress)
				{
					mol::account_info info;
					auto latest_error (ledger.store.account_get (transaction, account, info));
					assert (!latest_error);
					assert (info.head == block_a.hashables.previous);
					result.code = validate_message (account, hash, block_a.signature) ? mol::process_result::bad_signature : mol::process_result::progress; // Is this block signed correctly (Malformed)
					if (result.code == mol::process_result::progress)
					{
						ledger.store.block_put (transaction, hash, block_a);
						auto balance (ledger.balance (transaction, block_a.hashables.previous));
						ledger.store.representation_add (transaction, hash, balance);
						ledger.store.representation_add (transaction, info.rep_block, 0 - balance);
						ledger.change_latest (transaction, account, hash, hash, info.balance, info.block_count + 1);
						ledger.store.frontier_del (transaction, block_a.hashables.previous);
						ledger.store.frontier_put (transaction, hash, account);
						result.account = account;
						result.amount = 0;
						ledger.stats.inc (mol::stat::type::ledger, mol::stat::detail::change);
					}
				}
			}
		}
	}
}

void ledger_processor::send_block (mol::send_block const & block_a)
{
	auto hash (block_a.hash ());
	auto existing (ledger.store.block_exists (transaction, hash));
	result.code = existing ? mol::process_result::old : mol::process_result::progress; // Have we seen this block before? (Harmless)
	if (result.code == mol::process_result::progress)
	{
		auto previous (ledger.store.block_get (transaction, block_a.hashables.previous));
		result.code = previous != nullptr ? mol::process_result::progress : mol::process_result::gap_previous; // Have we seen the previous block already? (Harmless)
		if (result.code == mol::process_result::progress)
		{
			result.code = block_a.valid_predecessor (*previous) ? mol::process_result::progress : mol::process_result::block_position;
			if (result.code == mol::process_result::progress)
			{
				auto account (ledger.store.frontier_get (transaction, block_a.hashables.previous));
				result.code = account.is_zero () ? mol::process_result::fork : mol::process_result::progress;
				if (result.code == mol::process_result::progress)
				{
					result.code = validate_message (account, hash, block_a.signature) ? mol::process_result::bad_signature : mol::process_result::progress; // Is this block signed correctly (Malformed)
					if (result.code == mol::process_result::progress)
					{
						mol::account_info info;
						auto latest_error (ledger.store.account_get (transaction, account, info));
						assert (!latest_error);
						assert (info.head == block_a.hashables.previous);
						result.code = info.balance.number () >= block_a.hashables.balance.number () ? mol::process_result::progress : mol::process_result::negative_spend; // Is this trying to spend a negative amount (Malicious)
						if (result.code == mol::process_result::progress)
						{
							auto amount (info.balance.number () - block_a.hashables.balance.number ());
							ledger.store.representation_add (transaction, info.rep_block, 0 - amount);
							ledger.store.block_put (transaction, hash, block_a);
							ledger.change_latest (transaction, account, hash, info.rep_block, block_a.hashables.balance, info.block_count + 1);
							ledger.store.pending_put (transaction, mol::pending_key (block_a.hashables.destination, hash), { account, amount });
							ledger.store.frontier_del (transaction, block_a.hashables.previous);
							ledger.store.frontier_put (transaction, hash, account);
							result.account = account;
							result.amount = amount;
							result.pending_account = block_a.hashables.destination;
							ledger.stats.inc (mol::stat::type::ledger, mol::stat::detail::send);
						}
					}
				}
			}
		}
	}
}

void ledger_processor::receive_block (mol::receive_block const & block_a)
{
	auto hash (block_a.hash ());
	auto existing (ledger.store.block_exists (transaction, hash));
	result.code = existing ? mol::process_result::old : mol::process_result::progress; // Have we seen this block already?  (Harmless)
	if (result.code == mol::process_result::progress)
	{
		auto previous (ledger.store.block_get (transaction, block_a.hashables.previous));
		result.code = previous != nullptr ? mol::process_result::progress : mol::process_result::gap_previous;
		if (result.code == mol::process_result::progress)
		{
			result.code = block_a.valid_predecessor (*previous) ? mol::process_result::progress : mol::process_result::block_position;
			if (result.code == mol::process_result::progress)
			{
				result.code = ledger.store.block_exists (transaction, block_a.hashables.source) ? mol::process_result::progress : mol::process_result::gap_source; // Have we seen the source block already? (Harmless)
				if (result.code == mol::process_result::progress)
				{
					auto account (ledger.store.frontier_get (transaction, block_a.hashables.previous));
					result.code = account.is_zero () ? mol::process_result::gap_previous : mol::process_result::progress; //Have we seen the previous block? No entries for account at all (Harmless)
					if (result.code == mol::process_result::progress)
					{
						result.code = mol::validate_message (account, hash, block_a.signature) ? mol::process_result::bad_signature : mol::process_result::progress; // Is the signature valid (Malformed)
						if (result.code == mol::process_result::progress)
						{
							mol::account_info info;
							ledger.store.account_get (transaction, account, info);
							result.code = info.head == block_a.hashables.previous ? mol::process_result::progress : mol::process_result::gap_previous; // Block doesn't immediately follow latest block (Harmless)
							if (result.code == mol::process_result::progress)
							{
								mol::pending_key key (account, block_a.hashables.source);
								mol::pending_info pending;
								result.code = ledger.store.pending_get (transaction, key, pending) ? mol::process_result::unreceivable : mol::process_result::progress; // Has this source already been received (Malformed)
								if (result.code == mol::process_result::progress)
								{
									auto new_balance (info.balance.number () + pending.amount.number ());
									mol::account_info source_info;
									auto error (ledger.store.account_get (transaction, pending.source, source_info));
									assert (!error);
									ledger.store.pending_del (transaction, key);
									ledger.store.block_put (transaction, hash, block_a);
									ledger.change_latest (transaction, account, hash, info.rep_block, new_balance, info.block_count + 1);
									ledger.store.representation_add (transaction, info.rep_block, pending.amount.number ());
									ledger.store.frontier_del (transaction, block_a.hashables.previous);
									ledger.store.frontier_put (transaction, hash, account);
									result.account = account;
									result.amount = pending.amount;
									ledger.stats.inc (mol::stat::type::ledger, mol::stat::detail::receive);
								}
							}
						}
					}
					else
					{
						result.code = ledger.store.block_exists (transaction, block_a.hashables.previous) ? mol::process_result::fork : mol::process_result::gap_previous; // If we have the block but it's not the latest we have a signed fork (Malicious)
					}
				}
			}
		}
	}
}

void ledger_processor::open_block (mol::open_block const & block_a)
{
	auto hash (block_a.hash ());
	auto existing (ledger.store.block_exists (transaction, hash));
	result.code = existing ? mol::process_result::old : mol::process_result::progress; // Have we seen this block already? (Harmless)
	if (result.code == mol::process_result::progress)
	{
		auto source_missing (!ledger.store.block_exists (transaction, block_a.hashables.source));
		result.code = source_missing ? mol::process_result::gap_source : mol::process_result::progress; // Have we seen the source block? (Harmless)
		if (result.code == mol::process_result::progress)
		{
			result.code = mol::validate_message (block_a.hashables.account, hash, block_a.signature) ? mol::process_result::bad_signature : mol::process_result::progress; // Is the signature valid (Malformed)
			if (result.code == mol::process_result::progress)
			{
				mol::account_info info;
				result.code = ledger.store.account_get (transaction, block_a.hashables.account, info) ? mol::process_result::progress : mol::process_result::fork; // Has this account already been opened? (Malicious)
				if (result.code == mol::process_result::progress)
				{
					mol::pending_key key (block_a.hashables.account, block_a.hashables.source);
					mol::pending_info pending;
					result.code = ledger.store.pending_get (transaction, key, pending) ? mol::process_result::unreceivable : mol::process_result::progress; // Has this source already been received (Malformed)
					if (result.code == mol::process_result::progress)
					{
						result.code = block_a.hashables.account == mol::burn_account ? mol::process_result::opened_burn_account : mol::process_result::progress; // Is it burning 0 account? (Malicious)
						if (result.code == mol::process_result::progress)
						{
							mol::account_info source_info;
							auto error (ledger.store.account_get (transaction, pending.source, source_info));
							assert (!error);
							ledger.store.pending_del (transaction, key);
							ledger.store.block_put (transaction, hash, block_a);
							ledger.change_latest (transaction, block_a.hashables.account, hash, hash, pending.amount.number (), info.block_count + 1);
							ledger.store.representation_add (transaction, hash, pending.amount.number ());
							ledger.store.frontier_put (transaction, hash, block_a.hashables.account);
							result.account = block_a.hashables.account;
							result.amount = pending.amount;
							ledger.stats.inc (mol::stat::type::ledger, mol::stat::detail::open);
						}
					}
				}
			}
		}
	}
}

ledger_processor::ledger_processor (mol::ledger & ledger_a, MDB_txn * transaction_a) :
ledger (ledger_a),
transaction (transaction_a)
{
}
} // namespace

size_t mol::shared_ptr_block_hash::operator() (std::shared_ptr<mol::block> const & block_a) const
{
	auto hash (block_a->hash ());
	auto result (static_cast<size_t> (hash.qwords[0]));
	return result;
}

bool mol::shared_ptr_block_hash::operator() (std::shared_ptr<mol::block> const & lhs, std::shared_ptr<mol::block> const & rhs) const
{
	return *lhs == *rhs;
}

mol::ledger::ledger (mol::block_store & store_a, mol::stat & stat_a, mol::block_hash const & state_block_parse_canary_a, mol::block_hash const & state_block_generate_canary_a) :
store (store_a),
stats (stat_a),
check_bootstrap_weights (true),
state_block_parse_canary (state_block_parse_canary_a),
state_block_generate_canary (state_block_generate_canary_a)
{
}

// Sum the weights for each vote and return the winning block with its vote tally
std::pair<mol::uint128_t, std::shared_ptr<mol::block>> mol::ledger::winner (MDB_txn * transaction_a, mol::votes const & votes_a)
{
	auto tally_l (tally (transaction_a, votes_a));
	auto existing (tally_l.begin ());
	return std::make_pair (existing->first, existing->second);
}

std::map<mol::uint128_t, std::shared_ptr<mol::block>, std::greater<mol::uint128_t>> mol::ledger::tally (MDB_txn * transaction_a, mol::votes const & votes_a)
{
	std::unordered_map<std::shared_ptr<block>, mol::uint128_t, mol::shared_ptr_block_hash, mol::shared_ptr_block_hash> totals;
	// Construct a map of blocks -> vote total.
	for (auto & i : votes_a.rep_votes)
	{
		auto existing (totals.find (i.second));
		if (existing == totals.end ())
		{
			totals.insert (std::make_pair (i.second, 0));
			existing = totals.find (i.second);
			assert (existing != totals.end ());
		}
		auto weight_l (weight (transaction_a, i.first));
		existing->second += weight_l;
	}
	// Construction a map of vote total -> block in decreasing order.
	std::map<mol::uint128_t, std::shared_ptr<mol::block>, std::greater<mol::uint128_t>> result;
	for (auto & i : totals)
	{
		result[i.second] = i.first;
	}
	return result;
}

// Balance for account containing hash
mol::uint128_t mol::ledger::balance (MDB_txn * transaction_a, mol::block_hash const & hash_a)
{
	balance_visitor visitor (transaction_a, store);
	visitor.compute (hash_a);
	return visitor.result;
}

// Balance for an account by account number
mol::uint128_t mol::ledger::account_balance (MDB_txn * transaction_a, mol::account const & account_a)
{
	mol::uint128_t result (0);
	mol::account_info info;
	auto none (store.account_get (transaction_a, account_a, info));
	if (!none)
	{
		result = info.balance.number ();
	}
	return result;
}

mol::uint128_t mol::ledger::account_pending (MDB_txn * transaction_a, mol::account const & account_a)
{
	mol::uint128_t result (0);
	mol::account end (account_a.number () + 1);
	for (auto i (store.pending_begin (transaction_a, mol::pending_key (account_a, 0))), n (store.pending_begin (transaction_a, mol::pending_key (end, 0))); i != n; ++i)
	{
		mol::pending_info info (i->second);
		result += info.amount.number ();
	}
	return result;
}

mol::process_return mol::ledger::process (MDB_txn * transaction_a, mol::block const & block_a)
{
	ledger_processor processor (*this, transaction_a);
	block_a.visit (processor);
	return processor.result;
}

mol::block_hash mol::ledger::representative (MDB_txn * transaction_a, mol::block_hash const & hash_a)
{
	auto result (representative_calculated (transaction_a, hash_a));
	assert (result.is_zero () || store.block_exists (transaction_a, result));
	return result;
}

mol::block_hash mol::ledger::representative_calculated (MDB_txn * transaction_a, mol::block_hash const & hash_a)
{
	representative_visitor visitor (transaction_a, store);
	visitor.compute (hash_a);
	return visitor.result;
}

bool mol::ledger::block_exists (mol::block_hash const & hash_a)
{
	mol::transaction transaction (store.environment, nullptr, false);
	auto result (store.block_exists (transaction, hash_a));
	return result;
}

std::string mol::ledger::block_text (char const * hash_a)
{
	return block_text (mol::block_hash (hash_a));
}

std::string mol::ledger::block_text (mol::block_hash const & hash_a)
{
	std::string result;
	mol::transaction transaction (store.environment, nullptr, false);
	auto block (store.block_get (transaction, hash_a));
	if (block != nullptr)
	{
		block->serialize_json (result);
	}
	return result;
}

bool mol::ledger::is_send (MDB_txn * transaction_a, mol::state_block const & block_a)
{
	bool result (false);
	mol::block_hash previous (block_a.hashables.previous);
	if (!previous.is_zero ())
	{
		if (block_a.hashables.balance < balance (transaction_a, previous))
		{
			result = true;
		}
	}
	return result;
}

mol::block_hash mol::ledger::block_destination (MDB_txn * transaction_a, mol::block const & block_a)
{
	mol::block_hash result (0);
	mol::send_block const * send_block (dynamic_cast<mol::send_block const *> (&block_a));
	mol::state_block const * state_block (dynamic_cast<mol::state_block const *> (&block_a));
	if (send_block != nullptr)
	{
		result = send_block->hashables.destination;
	}
	else if (state_block != nullptr && is_send (transaction_a, *state_block))
	{
		result = state_block->hashables.link;
	}
	return result;
}

mol::block_hash mol::ledger::block_source (MDB_txn * transaction_a, mol::block const & block_a)
{
	// If block_a.source () is nonzero, then we have our source.
	// However, universal blocks will always return zero.
	mol::block_hash result (block_a.source ());
	mol::state_block const * state_block (dynamic_cast<mol::state_block const *> (&block_a));
	if (state_block != nullptr && !is_send (transaction_a, *state_block))
	{
		result = state_block->hashables.link;
	}
	return result;
}

// Vote weight of an account
mol::uint128_t mol::ledger::weight (MDB_txn * transaction_a, mol::account const & account_a)
{
	if (check_bootstrap_weights.load ())
	{
		auto blocks = store.block_count (transaction_a);
		if (blocks.sum () < bootstrap_weight_max_blocks)
		{
			auto weight = bootstrap_weights.find (account_a);
			if (weight != bootstrap_weights.end ())
			{
				return weight->second;
			}
		}
		else
		{
			check_bootstrap_weights = false;
		}
	}
	return store.representation_get (transaction_a, account_a);
}

// Rollback blocks until `block_a' doesn't exist
void mol::ledger::rollback (MDB_txn * transaction_a, mol::block_hash const & block_a)
{
	assert (store.block_exists (transaction_a, block_a));
	auto account_l (account (transaction_a, block_a));
	rollback_visitor rollback (transaction_a, *this);
	mol::account_info info;
	while (store.block_exists (transaction_a, block_a))
	{
		auto latest_error (store.account_get (transaction_a, account_l, info));
		assert (!latest_error);
		auto block (store.block_get (transaction_a, info.head));
		block->visit (rollback);
	}
}

// Return account containing hash
mol::account mol::ledger::account (MDB_txn * transaction_a, mol::block_hash const & hash_a)
{
	mol::account result;
	auto hash (hash_a);
	mol::block_hash successor (1);
	mol::block_info block_info;
	std::unique_ptr<mol::block> block (store.block_get (transaction_a, hash));
	while (!successor.is_zero () && block->type () != mol::block_type::state && store.block_info_get (transaction_a, successor, block_info))
	{
		successor = store.block_successor (transaction_a, hash);
		if (!successor.is_zero ())
		{
			hash = successor;
			block = store.block_get (transaction_a, hash);
		}
	}
	if (block->type () == mol::block_type::state)
	{
		auto state_block (dynamic_cast<mol::state_block *> (block.get ()));
		result = state_block->hashables.account;
	}
	else if (successor.is_zero ())
	{
		result = store.frontier_get (transaction_a, hash);
	}
	else
	{
		result = block_info.account;
	}
	assert (!result.is_zero ());
	return result;
}

// Return amount decrease or increase for block
mol::uint128_t mol::ledger::amount (MDB_txn * transaction_a, mol::block_hash const & hash_a)
{
	amount_visitor amount (transaction_a, store);
	amount.compute (hash_a);
	return amount.result;
}

// Return latest block for account
mol::block_hash mol::ledger::latest (MDB_txn * transaction_a, mol::account const & account_a)
{
	mol::account_info info;
	auto latest_error (store.account_get (transaction_a, account_a, info));
	return latest_error ? 0 : info.head;
}

// Return latest root for account, account number of there are no blocks for this account.
mol::block_hash mol::ledger::latest_root (MDB_txn * transaction_a, mol::account const & account_a)
{
	mol::account_info info;
	auto latest_error (store.account_get (transaction_a, account_a, info));
	mol::block_hash result;
	if (latest_error)
	{
		result = account_a;
	}
	else
	{
		result = info.head;
	}
	return result;
}

mol::checksum mol::ledger::checksum (MDB_txn * transaction_a, mol::account const & begin_a, mol::account const & end_a)
{
	mol::checksum result;
	auto error (store.checksum_get (transaction_a, 0, 0, result));
	assert (!error);
	return result;
}

void mol::ledger::dump_account_chain (mol::account const & account_a)
{
	mol::transaction transaction (store.environment, nullptr, false);
	auto hash (latest (transaction, account_a));
	while (!hash.is_zero ())
	{
		auto block (store.block_get (transaction, hash));
		assert (block != nullptr);
		std::cerr << hash.to_string () << std::endl;
		hash = block->previous ();
	}
}

bool mol::ledger::state_block_parsing_enabled (MDB_txn * transaction_a)
{
	return store.block_exists (transaction_a, state_block_parse_canary);
}

bool mol::ledger::state_block_generation_enabled (MDB_txn * transaction_a)
{
	return state_block_parsing_enabled (transaction_a) && store.block_exists (transaction_a, state_block_generate_canary);
}

void mol::ledger::checksum_update (MDB_txn * transaction_a, mol::block_hash const & hash_a)
{
	mol::checksum value;
	auto error (store.checksum_get (transaction_a, 0, 0, value));
	assert (!error);
	value ^= hash_a;
	store.checksum_put (transaction_a, 0, 0, value);
}

void mol::ledger::change_latest (MDB_txn * transaction_a, mol::account const & account_a, mol::block_hash const & hash_a, mol::block_hash const & rep_block_a, mol::amount const & balance_a, uint64_t block_count_a, bool is_state)
{
	mol::account_info info;
	auto exists (!store.account_get (transaction_a, account_a, info));
	if (exists)
	{
		checksum_update (transaction_a, info.head);
	}
	else
	{
		assert (store.block_get (transaction_a, hash_a)->previous ().is_zero ());
		info.open_block = hash_a;
	}
	if (!hash_a.is_zero ())
	{
		info.head = hash_a;
		info.rep_block = rep_block_a;
		info.balance = balance_a;
		info.modified = mol::seconds_since_epoch ();
		info.block_count = block_count_a;
		store.account_put (transaction_a, account_a, info);
		if (!(block_count_a % store.block_info_max) && !is_state)
		{
			mol::block_info block_info;
			block_info.account = account_a;
			block_info.balance = balance_a;
			store.block_info_put (transaction_a, hash_a, block_info);
		}
		checksum_update (transaction_a, hash_a);
	}
	else
	{
		store.account_del (transaction_a, account_a);
	}
}

std::unique_ptr<mol::block> mol::ledger::successor (MDB_txn * transaction_a, mol::uint256_union const & root_a)
{
	mol::block_hash successor (0);
	if (store.account_exists (transaction_a, root_a))
	{
		mol::account_info info;
		auto error (store.account_get (transaction_a, root_a, info));
		assert (!error);
		successor = info.open_block;
	}
	else
	{
		successor = store.block_successor (transaction_a, root_a);
	}
	std::unique_ptr<mol::block> result;
	if (!successor.is_zero ())
	{
		result = store.block_get (transaction_a, successor);
	}
	assert (successor.is_zero () || result != nullptr);
	return result;
}

std::unique_ptr<mol::block> mol::ledger::forked_block (MDB_txn * transaction_a, mol::block const & block_a)
{
	assert (!store.block_exists (transaction_a, block_a.hash ()));
	auto root (block_a.root ());
	assert (store.block_exists (transaction_a, root) || store.account_exists (transaction_a, root));
	std::unique_ptr<mol::block> result (store.block_get (transaction_a, store.block_successor (transaction_a, root)));
	if (result == nullptr)
	{
		mol::account_info info;
		auto error (store.account_get (transaction_a, root, info));
		assert (!error);
		result = store.block_get (transaction_a, info.open_block);
		assert (result != nullptr);
	}
	return result;
}
