#pragma once
/**
 * The 'TimingGraph' class represents a timing graph.
 *
 * Logically the timing graph is a directed graph connecting Primary Inputs (nodes with no
 * fan-in, e.g. circuit inputs Flip-Flop Q pins) to Primary Outputs (nodes with no fan-out,
 * e.g. circuit outputs, Flip-Flop D pins), connecting through intermediate nodes (nodes with
 * both fan-in and fan-out, e.g. combinational logic).
 *
 * To make performing the forward/backward traversals through the timing graph easier, we actually
 * store all edges as bi-directional edges.
 *
 * NOTE: We store only the static connectivity and node information in the 'TimingGraph' class.
 *       Other dynamic information (edge delays, node arrival/required times) is stored seperately.
 *       This means that most actions opearting on the timing graph (e.g. TimingAnalyzers) only
 *       require read-only access to the timing graph.
 *
 * Accessing Graph Data
 * ======================
 * For performance reasons (see Implementation section for details) we store all graph data
 * in the 'TimingGraph' class, and do not use separate edge/node objects.  To facilitate this,
 * each node and edge in the graph is given a unique identifier (e.g. NodeId, EdgeId). These
 * ID's can then be used to access the required data through the appropriate member function.
 *
 * Implementation
 * ================
 * The 'TimingGraph' class represents the timing graph in a "Struct of Arrays (SoA)" manner,
 * rather than the more typical "Array of Structs (AoS)" data layout.
 *
 * By using a SoA layout we keep all data for a particular field (e.g. node types) in contiguous
 * memory.  Using an AoS layout the various fields accross nodes would *not* be contiguous
 * (although the different fields within each object (e.g. a TimingNode class) would be contiguous.
 * Since we typically perform operations on particular fields accross nodes the SoA layout performs
 * better (and enables memory ordering optimizations). The edges are also stored in a SOA format.
 *
 * The SoA layout also motivates the ID based approach, which allows direct indexing into the required
 * vector to retrieve data.
 *
 * Memory Ordering Optimizations
 * ===============================
 * SoA also allows several additional memory layout optimizations.  In particular,  we know the
 * order that a (serial) timing analyzer will walk the timing graph (i.e. level-by-level, from the
 * start to end node in each level).
 *
 * Using this information we can re-arrange the node and edge data to match this traversal order.
 * This greatly improves caching behaviour, since pulling in data for one node immediately pulls
 * in data for the next node/edge to be processed. This exploits both spatial and temporal locality,
 * and ensures that each cache line pulled into the cache will (likely) be accessed multiple times
 * before being evicted.
 *
 * Note that performing these optimizations is currently done explicity by calling the optimize_edge_layout()
 * and optimize_node_layout() member functions.  In the future (particularily if incremental modification
 * support is added), it may be a good idea apply these modifications automatically as needed.
 *
 */
#include <vector>
#include <iosfwd>

#include "tatum_range.hpp"
#include "tatum_linear_map.hpp"
#include "timing_graph_fwd.hpp"

namespace tatum {

class TimingGraph {
    public: //Public types
        //Iterators
        typedef tatum::util::linear_map<EdgeId,EdgeId>::const_iterator edge_iterator;
        typedef tatum::util::linear_map<NodeId,NodeId>::const_iterator node_iterator;
        typedef tatum::util::linear_map<LevelId,LevelId>::const_iterator level_iterator;
        typedef tatum::util::linear_map<LevelId,LevelId>::const_reverse_iterator reverse_level_iterator;

        //Ranges
        typedef tatum::util::Range<node_iterator> node_range;
        typedef tatum::util::Range<edge_iterator> edge_range;
        typedef tatum::util::Range<level_iterator> level_range;
        typedef tatum::util::Range<reverse_level_iterator> reverse_level_range;
    public: //Public accessors
        /*
         * Node data accessors
         */
        ///\param id The id of a node
        ///\returns The type of the node
        TN_Type node_type(const NodeId id) const { return node_types_[id]; }

        ///\param id The id of a node
        ///\returns The clock domain of the node
        DomainId node_clock_domain(const NodeId id) const { return node_clock_domains_[id]; }

        ///\param id The id of a node
        ///\returns Whether the is the source of a clock
        bool node_is_clock_source(const NodeId id) const { return node_is_clock_source_[id]; }

        ///\param id The node id
        ///\returns A range of all out-going edges the node drives
        edge_range node_out_edges(const NodeId id) const { return tatum::util::make_range(node_out_edges_[id].begin(), node_out_edges_[id].end()); }

        ///\param id The node id
        ///\returns A range of all in-coming edges the node drives
        edge_range node_in_edges(const NodeId id) const { return tatum::util::make_range(node_in_edges_[id].begin(), node_in_edges_[id].end()); }

        /*
         * Edge accessors
         */
        ///\param id The id of an edge
        ///\returns The node id of the edge's sink
        NodeId edge_sink_node(const EdgeId id) const { return edge_sink_nodes_[id]; }

        ///\param id The id of an edge
        ///\returns The node id of the edge's source (driver)
        NodeId edge_src_node(const EdgeId id) const { return edge_src_nodes_[id]; }

        /*
         * Level accessors
         */
        ///\param level_id The level index in the graph
        ///\pre The graph must be levelized.
        ///\returns A range containing the nodes in the level
        ///\see levelize()
        node_range level_nodes(const LevelId level_id) const { return tatum::util::make_range(level_nodes_[level_id].begin(),
                                                                                        level_nodes_[level_id].end()); }

        ///\pre The graph must be levelized.
        ///\returns A range containing the nodes which are primary inputs
        ///\see levelize()
        node_range primary_inputs() const { return tatum::util::make_range(level_nodes_[LevelId(0)].begin(), level_nodes_[LevelId(0)].end()); } //After levelizing PIs will be 1st level

        ///\pre The graph must be levelized.
        ///\returns A range containing the nodes which are primary outputs
        ///\warning The primary outputs may be on different levels of the graph
        ///\see levelize()
        node_range primary_outputs() const { return tatum::util::make_range(primary_outputs_.begin(), primary_outputs_.end()); }

        /*
         * Graph aggregate accessors
         */
        //\returns A range containing all nodes in the graph
        node_range nodes() const { return tatum::util::make_range(node_ids_.begin(), node_ids_.end()); }

        //\returns A range containing all edges in the graph
        edge_range edges() const { return tatum::util::make_range(edge_ids_.begin(), edge_ids_.end()); }

        //\returns A range containing all levels in the graph
        level_range levels() const { return tatum::util::make_range(level_ids_.begin(), level_ids_.end()); }

        //\returns A range containing all levels in the graph in *reverse* order
        reverse_level_range reversed_levels() const { return tatum::util::make_range(level_ids_.rbegin(), level_ids_.rend()); }

    public: //Mutators
        /*
         * Graph modifiers
         */
        ///Adds a node to the timing graph
        ///\param type The type of the node to be added
        ///\param clock_domain The clock domain id of the node to be added
        ///\param is_clk_src Identifies if the node to be added is the source of a clock
        ///\warning Graph will likely need to be re-levelized after modification
        NodeId add_node(const TN_Type type, const DomainId clock_domain, const bool is_clk_src);

        ///Adds an edge to the timing graph
        ///\param src_node The node id of the edge's driving node
        ///\param sink_node The node id of the edge's sink node
        ///\pre The src_node and sink_node must have been already added to the graph
        ///\warning Graph will likely need to be re-levelized after modification
        EdgeId add_edge(const NodeId src_node, const NodeId sink_node);

        /*
         * Graph-level modification operations
         */
        ///Levelizes the graph.
        ///\post The graph topologically ordered (i.e. the level of each node is known)
        ///\post The primary outputs have been identified
        void levelize();

        /*
         * Memory layout optimization operations
         */
        ///Optimizes the memory layout of edges in the graph by re-ordering them
        ///for improved spatial/temporal cache locality.
        ///\pre The graph must be levelized
        ///\warning Old edge ids are invalidated
        ///\returns A mapping from old to new edge ids
        ///\see levelize()
        tatum::util::linear_map<EdgeId,EdgeId> optimize_edge_layout();

        ///Optimizes the memory layout of nodes in the graph by re-ordering them
        ///for improved spatial/temporal cache locality.
        ///\pre The graph must be levelized
        ///\warning Old node ids are invalidated
        ///\returns A mapping from old to new node ids
        ///\see levelize()
        tatum::util::linear_map<NodeId,NodeId> optimize_node_layout();

    private: //Internal helper functions
        bool valid_node_id(const NodeId node_id);
        bool valid_edge_id(const EdgeId edge_id);
        bool valid_level_id(const LevelId level_id);

    private: //Data
        /*
         * For improved memory locality, we use a Struct of Arrays (SoA)
         * data layout, rather than Array of Structs (AoS)
         */
        //Node data
        tatum::util::linear_map<NodeId,NodeId> node_ids_; //The node IDs in the graph
        tatum::util::linear_map<NodeId,TN_Type> node_types_; //Type of node [0..num_nodes()-1]
        tatum::util::linear_map<NodeId,DomainId> node_clock_domains_; //Clock domain of node [0..num_nodes()-1]
        tatum::util::linear_map<NodeId,std::vector<EdgeId>> node_out_edges_; //Out going edge IDs for node 'node_id' [0..num_nodes()-1][0..num_node_out_edges(node_id)-1]
        tatum::util::linear_map<NodeId,std::vector<EdgeId>> node_in_edges_; //Incomiing edge IDs for node 'node_id' [0..num_nodes()-1][0..num_node_in_edges(node_id)-1]
        tatum::util::linear_map<NodeId,bool> node_is_clock_source_; //Indicates if a node is the start of clock [0..num_nodes()-1]

        //Edge data
        tatum::util::linear_map<EdgeId,EdgeId> edge_ids_; //The edge IDs in the graph
        tatum::util::linear_map<EdgeId,NodeId> edge_sink_nodes_; //Sink node for each edge [0..num_edges()-1]
        tatum::util::linear_map<EdgeId,NodeId> edge_src_nodes_; //Source node for each edge [0..num_edges()-1]

        //Auxilary graph-level info, filled in by levelize()
        tatum::util::linear_map<LevelId,LevelId> level_ids_; //The level IDs in the graph
        tatum::util::linear_map<LevelId,std::vector<NodeId>> level_nodes_; //Nodes in each level [0..num_levels()-1]
        std::vector<NodeId> primary_outputs_; //Primary output nodes of the timing graph.
                                              //NOTE: we track this separetely (unlike Primary Inputs) since these are
                                              //      scattered through the graph and do not exist on a single level
};

} //namepsace
