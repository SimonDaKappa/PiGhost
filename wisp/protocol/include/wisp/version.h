#ifndef WISP_VERSION_H
#define WISP_VERSION_H

#include <stdint.h>

/**
 * WISP_PROTOCOL_MAGIC - identifies the wire/framing format itself
 *
 * Bumped only when the control-message framing or an existing message's binary layout
 * changes in a way that makes an old and new peer unable to even parse each other's
 * bytes (e.g. header size, field order/width). A client and server built against
 * different magics cannot safely exchange a single byte of control traffic, so this
 * is checked before anything else in CONNECT and rejected with no negotiation
 * possible.
 */
#define WISP_PROTOCOL_MAGIC 0x57495350u

/**
 * WISP_PROTOCOL_VERSION_MAJOR - the wisp_protocol api release this build implements
 *
 * Bumped when message *semantics* change in an incompatible way (new required
 * fields, changed behavior for an existing message kind) even if the magic/framing
 * is untouched. A client and server with matching magic but mismatched major
 * cannot be assumed to agree on what any given message means, so this is also
 * rejected outright rather than negotiated.
 *
 * Finer-grained, per-feature compatibility (does this build understand feature X,
 * at what version) is a separate, additive concern -- see wisp_feature_desc_t.
 */
#define WISP_PROTOCOL_VERSION_MAJOR 1u

/**
 * WISP_PROTOCOL_VERSION_MINOR - backwards-compatible major version additions
 * 
 * Bumped when new, backwards-compatible features are added to the protocol, without
 * changing the existing message semantics. Clients and servers with matching magic
 * and major version but differing minor versions can still communicate, with the
 * understanding that the newer minor version may support additional optional
 * features not understood by the older one. 
 * 
 * Closely coupled with changes in a @wisp_feature_desc_t
 */
#define WISP_PROTOCOL_VERSION_MINOR 1u

/**
 * WISP_MAX_FEATURES - upper bound on distinct feature ids exchanged during CONNECT
 */
#define WISP_MAX_FEATURES 16

/**
 * struct wisp_feature_desc_t - one independently-versioned optional capability
 * @id:         stable identifier for this feature, never reused/renumbered once shipped
 * @version:    monotonically increasing revision of @id's behavior; a higher version
 *              must always be a superset/refinement of every lower version's behavior
 * @min_major:  the lowest WISP_PROTOCOL_VERSION_MAJOR a peer must be running to even
 *              attempt handling this feature at all
 * @min_minor:  the lowest WISP_PROTOCOL_VERSION_MINOR (at @min_major) a peer must be
 *              running to handle this feature
 *
 * @min_major/@min_minor let a peer discard an advertised feature purely from the
 * connection's already-negotiated protocol version, without inspecting @version --
 * e.g. a server can skip emitting a feature descriptor entirely for a client whose
 * negotiated minor is below @min_minor, since that client's wire format cannot even
 * carry the messages/fields the feature would add. @version then governs the
 * feature's own independent evolution *within* whatever peers do speak it (finer
 * grained than the coarse protocol minor, which only tracks "did this feature's wire
 * shape get introduced at all").
 *
 * Not yet exchanged on the wire or consulted anywhere -- this type only scaffolds the
 * planned client/server *feature* compatibility layer (distinct from the
 * magic/major/minor checks above, which gate the wire format itself).
 *
 * The intent: CONNECT/MODE each grow a `wisp_feature_desc_t
 * features[WISP_MAX_FEATURES]` advertisement of every feature id the sender understands
 * and at what version. For each id present in both sets, the negotiated version is
 * min(client_version, server_version); an id only one side lists is simply not
 * available for that session. The server must never emit a message that depends on a
 * feature id/version the client did not end up with in this intersection.
 */
typedef struct {
  uint32_t id;
  uint32_t version;
  uint32_t min_major;
  uint32_t min_minor;
} wisp_feature_desc_t;

#endif /* WISP_VERSION_H */