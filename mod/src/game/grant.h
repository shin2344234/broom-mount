#pragma once

// Broomy joins the player's roster once per save, automatically. After a
// save loads, the plugin looks through the server's roster for Broomy's row.
// When Broomy is not there, it calls the server's hire, on the server's own
// thread, for an unowned wild horse the server has loaded and, for the length
// of that one hire, points the horse's characterinfo row at Broomy's. The
// hire reads that row, so Broomy is who joins.
namespace bm::grant
{
    // From the worker thread once the log is claimed. The hooks and the
    // roster check run either way: the call fixes (a fainted Broomy, a first
    // call to a Broomy already out) need them. Only with give is a save
    // without Broomy given one.
    bool Install(bool give);
    // From the worker thread, twice a second.
    void Tick();
}
